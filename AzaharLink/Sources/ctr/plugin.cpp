#include "ctr/plugin.hpp"

#include <CTRPluginFramework.hpp>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>

#include "azl/log.hpp"
#include "azl/plugin_compare.hpp"
#include "azl/rcvbuf.h"
#include "azl/soc_plan.hpp"
#include "azl/stats.hpp"
#include "azl/text_wrap.hpp"
#include "azl/ticks.h"
#include "azl/wallclock.hpp"
#include "ctr/boot_diag.hpp"
#include "ctr/soc.hpp"
#include "ctr/teardown.hpp"

// (libctru's weak hid function; Sources/main.cpp overrides it. No header of libctru declares it.)
extern "C" bool hidShouldUseIrrst(void);

extern "C" int azl_enet_socket_buffers; // Sources/enet/enet_3ds.c
extern "C" int azl_enet_rcvbuf_bytes;

namespace azl {
namespace ctr {

namespace {

using CTRPluginFramework::File;

// The plugin's own files - the settings and the logs - live with the app's, in /3ds/3DSLink/ (the plugin binaries stay where
// Luma3DS loads them from, /luma/plugins/<title id>/). The settings used to be in /luma/plugins/AzaharLink.ini: that file is
// still read when the new one is missing (an app older than this build wrote it there; the app moves it the first time it runs).
constexpr const char* kDataDir = "/3ds/3DSLink/";
constexpr const char* kConfigPath = "/3ds/3DSLink/AzaharLink.ini";
constexpr const char* kOldConfigPath = "/luma/plugins/AzaharLink.ini";
// The memory figure of the last launch (title id, KB used, KB free of the application region at the start): written with the
// log on only, read at the next start (CompareMemoryWithLastLaunch).
constexpr const char* kMemPath = "/3ds/3DSLink/AzaharLink.mem";

/// Makes /3ds/3DSLink (and /3ds) when they are missing: the files of the plugin have nowhere to go otherwise.
void MakeDataDir() {
    using CTRPluginFramework::Directory;
    if (Directory::IsExists("/3ds/3DSLink") != 1) {
        Directory::Create("/3ds");
        Directory::Create("/3ds/3DSLink");
    }
}
// The longest line of a message on the screen (the overlay is one line of the top screen wide).
constexpr std::size_t kToastChars = 58;
// ... and of the message box of the details (about 45 characters).
constexpr std::size_t kDetailChars = 44;
constexpr u32 kHintIntervalMs = 60000;
constexpr u32 kLogServiceIntervalMs = 900; // (the writer wakes every 1000 ms with the log off)
constexpr u32 kClockLineIntervalMs = 60000; // a "clock:" line (the console's wall clock) in the log and the capture
constexpr u32 kStatsIntervalMs = 5000;
// The log writer thread: above the game's threads (0x30-0x31) and the network thread's (0x2F). A thread that holds a lock the
// game's threads take must not be preemptable by them: a LightLock has no priority inheritance, and with the writer at 0x38
// (the lowest, until 2026-10-20) a game thread that found a lock in the preempted writer's hand waited for every thread above
// the writer to leave the core, which a game that spins never does (Tekken froze twice in one session with the log on, at the
// publisher's screen and on leaving a lobby; nothing froze with it off). Its CPU is small (formatting: a 64 KB chunk of the
// capture is about a millisecond) and its writes block it in the file system, which is when the game runs.
constexpr int kLogWriterPriority = 0x2E;
constexpr u32 kNetStopTimeoutMs = 400;
// Process events of the framework (Plugin::NoteProcessEvent, CTRPluginFramework's Process::Event): the index of
// EXIT and of the two sleep ones.
constexpr int kEventExit = 0;
constexpr int kEventSleepEnter = 1;
constexpr int kEventSleepExit = 2;
constexpr u32 kLogStopTimeoutMs = 400;

// The WiFi MAC address of the console, as exposed by the kernel in the shared page, and the two bytes
// after the 6 of the MAC (the link strength 0..3 and the network state; their meaning on this system is
// what the log of a lid close or a lost link is going to show).
constexpr u32 kSharedPageWifiMac = 0x1FF81060;
constexpr u32 kSharedPageWifiStrength = 0x1FF81066;
constexpr u32 kSharedPageWifiState = 0x1FF81067;
constexpr u32 kLinkSampleIntervalMs = 50;
// The part of the plugin heap the framework keeps for itself (the plugin header says how large the heap is).
constexpr unsigned long kFrameworkReserved = 0xAB000;
// Warnings go on screen at most once a minute per kind.
constexpr u32 kWarnIntervalMs = 60000;
enum WarnKind { kWarnHeap = 0, kWarnSocket, kWarnTxDrops, kWarnOverflow, kWarnHle, kWarnDebugLoad, kWarnSilence };
constexpr u32 kMemIntervalMs = 30000;
constexpr long kMinFreeHeapBytes = 512 * 1024;

constexpr const char* kDefaultNickname = "3DS Player";

/// The console's wall clock (osGetTime), for LogFiles.
u64 ConsoleClockMs() {
    return osGetTime();
}

std::string MacToString(const Mac& m) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4],
                  m[5]);
    return buf;
}

/// Turns any text into something the room accepts as a nickname: [ a-zA-Z0-9._-]{4,20}.
std::string SanitizeNickname(const std::string& in) {
    std::string out;
    for (char c : in) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == ' ' || c == '.' || c == '_' || c == '-';
        out += ok ? c : '_';
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    while (!out.empty() && out.front() == ' ') {
        out.erase(out.begin());
    }
    if (out.size() > 20) {
        out.resize(20);
    }
    // A name with nothing but substituted characters tells the other players nothing.
    bool meaningful = false;
    for (char c : out) {
        if (c != '_' && c != ' ' && c != '.' && c != '-') {
            meaningful = true;
        }
    }
    if (!meaningful) {
        return "";
    }
    if (out.size() < 4) {
        out += " 3DS";
    }
    return out;
}

/// Makes a game title received from another player safe to show and to forward.
std::string SanitizeTitle(const std::string& in) {
    std::string out;
    for (char c : in) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u == '\n' || u == '\r' || u == '\t') {
            if (!out.empty() && out.back() != ' ') {
                out += ' '; // a line break in a title is a space
            }
        } else if (u >= 0x20 && u != 0x7F) {
            out += c;
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    if (out.size() > 80) {
        // Cut at 80 bytes, but never in the middle of a UTF-8 sequence.
        std::size_t cut = 80;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        out.resize(cut);
    }
    return out;
}

void AppendUtf8(std::string& out, u16 c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

/// UTF-16 text from an SMDH field -> UTF-8, newlines flattened. Surrogates become '?'.
std::string Utf16ToUtf8(const u16* text, std::size_t max_units) {
    std::string out;
    for (std::size_t i = 0; i < max_units && text[i]; ++i) {
        const u16 c = text[i];
        if (c == '\n' || c == '\r') {
            if (!out.empty() && out.back() != ' ') {
                out += ' ';
            }
        } else if (c >= 0xD800 && c < 0xE000) {
            out += '?';
        } else {
            AppendUtf8(out, c);
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

/// Bytes the plugin's malloc heap can hold: the heap Luma gave the plugin (its header says how large it is)
/// less the first 0xAB000 that the framework uses for itself. 0 if the header is not there.
unsigned long HeapCapacity() {
    const CTRPluginFramework::PluginHeader* header = CTRPluginFramework::FwkSettings::Header;
    if (!header || header->heapSize <= kFrameworkReserved || header->heapSize > 0x1000000) {
        return 0;
    }
    return header->heapSize - kFrameworkReserved;
}

/// "0x C8804478 [lvl 25 sum 4 mod 17 desc 120]": the fields of a 3DS result code.
std::string DescribeResult(Result rc) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "0x%08lx [lvl %lu sum %lu mod %lu desc %lu]",
                  static_cast<unsigned long>(rc), static_cast<unsigned long>((rc >> 27) & 0x1F),
                  static_cast<unsigned long>((rc >> 21) & 0x3F),
                  static_cast<unsigned long>((rc >> 10) & 0xFF),
                  static_cast<unsigned long>(rc & 0x3FF));
    return buf;
}

constexpr u32 kSmdhTitlesEnd = 8 + 16 * 0x200; // header + the 16 language entries

/// Reads the game's name out of an open SMDH (icon) file. English first, because that is what Azahar
/// sends to the room for the same game; then the console's language, Japanese, anything else.
std::string TitleFromSmdh(Handle file, u8 console_language, const char* what) {
    std::string title;
    u64 size = 0;
    const Result size_rc = FSFILE_GetSize(file, &size);
    // Read the WHOLE file (0x36C0 bytes for an SMDH, what Checkpoint does): on the console the read
    // of a shorter, odd-sized prefix (0x2008) failed with FS error 761 (0xE0C046F9) although the
    // open and GetSize worked.
    const u32 want = (R_SUCCEEDED(size_rc) && size >= kSmdhTitlesEnd && size <= 0x10000)
                         ? static_cast<u32>(size)
                         : 0x36C0;
    u8* buffer = new (std::nothrow) u8[want];
    if (!buffer) {
        return title;
    }
    std::memset(buffer, 0, want);
    u32 read = 0;
    const Result read_rc = FSFILE_Read(file, &read, 0, buffer, want);
    Log("game: icon[%s] size 0x%lx (%s) read 0x%lx (%s) magic %02x%02x%02x%02x", what,
        static_cast<unsigned long>(size), DescribeResult(size_rc).c_str(),
        static_cast<unsigned long>(read), DescribeResult(read_rc).c_str(), buffer[0], buffer[1],
        buffer[2], buffer[3]);
    if (R_SUCCEEDED(read_rc) && read >= 8 + 2 * 0x200 && std::memcmp(buffer, "SMDH", 4) == 0) {
        u8 order[2 + 1 + 16];
        int n = 0;
        order[n++] = 1; // English
        if (console_language <= 15 && console_language != 1) {
            order[n++] = console_language;
        }
        order[n++] = 0; // Japanese
        for (u8 i = 2; i < 16; ++i) {
            order[n++] = i;
        }
        for (int i = 0; i < n && title.empty(); ++i) {
            const u32 offset = 8 + order[i] * 0x200;
            if (offset + 0x80 > read) {
                continue; // that entry was not read
            }
            u16 units[0x40];
            std::memcpy(units, buffer + offset, sizeof(units)); // the short description
            title = Utf16ToUtf8(units, 0x40);
        }
    }
    delete[] buffer;
    return title;
}

/// Source 1: the process's own ExeFS "icon" through the "self NCCH" archive (0x3). No title id, no
/// media type, no extra rights needed.
std::string ReadIconSelfNcch(u8 language) {
    const FS_Path archive = {PATH_EMPTY, 1, ""};
    u32 file_words[3] = {2 /* ExeFS / system menu data */, 0x6E6F6369 /* "icon" */, 0};
    const FS_Path file = {PATH_BINARY, sizeof(file_words), file_words};
    Handle handle = 0;
    const Result rc = FSUSER_OpenFileDirectly(&handle, ARCHIVE_ROMFS, archive, file, FS_OPEN_READ, 0);
    if (R_FAILED(rc)) {
        Log("game: icon[selfncch] open %s", DescribeResult(rc).c_str());
        return "";
    }
    const std::string title = TitleFromSmdh(handle, language, "selfncch");
    FSFILE_Close(handle);
    return title;
}

// What the plugin does with the Download Play services (dlp:FKCL, dlp:SRVR) of a game, by title id (DlpGameMode, azl/dlp_hub.hpp):
//   ProbeOnly: the games whose wireless mode (UDS, every player with their own copy) starts by asking dlp:FKCL whether the wireless
//     is on and then never uses Download Play. dlp:FKCL is answered by the plugin (azl/dlp_fkcl_hle.hpp); dlp:SRVR stays the real one.
//     Another game may need the same as Battle Royale: to add one, put its title id here and check in its log that the only
//     dlp:FKCL commands are Initialize / InitializeWithName and Finalize (anything else is refused and written as
//     "dlp: dlp:FKCL command 0x.. refused").
//   Lobby: the games whose wireless mode goes on with the Download Play proper: Kirby Triple Deluxe and Kirby Planet Robobot (tried
//     with the probe on 2026-10-28: after Initialize they ask GetChannels, scan for a host and join; the probe refuses those
//     ("invalid state") and the game answers with an error screen, 0xE0A0AC01). The room carries their lobby: BOTH services are
//     served by the plugin's DlpHub (azl/dlp_hub.hpp), the host's announcement and the four messages of the creator go over the
//     room like the game's own UDS frames, and the engine is given back to the game's nwm::UDS whole when the game starts it.
// A game that is not listed keeps the real services, as before (Zelda Tri Force Heroes has no entry). Only the games in Lobby mode get
// a hub: nothing of it exists in the others.
struct DlpGame {
    u64 title_id;
    DlpGameMode mode;
};
constexpr DlpGame kDlpGames[] = {
    {0x00040000001C2100ull, DlpGameMode::ProbeOnly}, // Kirby Battle Royale (seen: InitializeWithName, Finalize 1 ms later, then nwm::UDS only)
    {0x00040000001C2000ull, DlpGameMode::ProbeOnly}, // Kirby Battle Royale, another region's title id (not seen)
    {0x000400000010C000ull, DlpGameMode::Lobby},     // Kirby Triple Deluxe (seen on a console: dlp:FKCL InitializeWithName, GetChannels, then the scan)
    {0x000400000010BF00ull, DlpGameMode::Lobby},     // Kirby Triple Deluxe, another region's title id (from a catalogue, not seen)
    {0x0004000000183600ull, DlpGameMode::Lobby},     // Kirby Planet Robobot (seen on a console; the ids of its other regions are not known here)
};

DlpGameMode DlpModeOf(u64 title_id) {
    for (const DlpGame& game : kDlpGames) {
        if (game.title_id == title_id) {
            return game.mode;
        }
    }
    return DlpGameMode::None;
}

/// A game of the probe's kind (Battle Royale): dlp:FKCL is answered by the plugin's probe, nothing else of Download Play is.
bool AsksDlpOnlyForTheWireless(u64 title_id) {
    return DlpModeOf(title_id) == DlpGameMode::ProbeOnly;
}

/// FNV-1a, 64 bits: the friend code seed of the lobby is the hash of the console's MAC, so that it is the console's own (two
/// consoles that enter the same lobby in the same round with one seed and one name would adopt the same Logoff) and the same
/// for as long as the session lasts.
u64 Fnv1a64(const u8* data, std::size_t size) {
    u64 hash = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

/// The IpcResolver of the lobby (what DlpService maps the game's buffers with): the plugin lives in the game's address space, so an
/// address the game put in a request is the pointer itself; what is refused is an address that cannot be one - null, below the code (0x00100000), at or past
/// the end of the application's range (0x40000000), or a range that wraps. Runs on the game's thread inside the IPC call (some
/// have 464 bytes below it): no local, no library call, at most 76 bytes of stack (Tests/stack_usage.sh).
u8* MapGameAddress(void* /*ctx*/, u32 address, u32 size) {
    constexpr u32 kLow = 0x00100000;
    constexpr u32 kHigh = 0x40000000;
    if (address < kLow || address >= kHigh || size > kHigh - address) {
        return nullptr;
    }
    return reinterpret_cast<u8*>(address);
}

/// The hub of a game in Lobby mode, or null (no memory: the game keeps the real services). `name` of the lobby's default player is
/// "3DS Link" (the game's InitializeWithName replaces it).
std::unique_ptr<DlpHub> MakeDlpHub(UdsEngine* engine, KernelOps* kernel, u64 game_tid, const Mac& console_mac) {
    LobbyConfig config = {};
    config.game_tid = game_tid;
    config.friend_seed = Fnv1a64(console_mac.data(), console_mac.size());
    static const char kDefaultName[] = "3DS Link";
    for (std::size_t i = 0; i < kNameUnits && kDefaultName[i] != '\0'; ++i) {
        config.name[i] = static_cast<u16>(kDefaultName[i]);
    }
    IpcResolver resolver;
    resolver.ctx = nullptr;
    resolver.map = &MapGameAddress;
    return std::unique_ptr<DlpHub>(new (std::nothrow) DlpHub(engine, kernel, config, resolver));
}

// ---------------------------------------------------------------------------------------------
// Is the plugin that runs in this game the one 3DS Link keeps in /3ds/3DSLink/? (azl/plugin_compare.hpp)
// ---------------------------------------------------------------------------------------------

constexpr const char* kMasterPluginPath = "/3ds/3DSLink/AzaharLink.3gx";
constexpr u64 kMasterCheckAfterMs = 25000; // (a while after the start: the game is loading from the same card before that)

/// The first 12 bytes of a file and its size. false when it is not there or cannot be read.
bool ReadPluginHead(const std::string& path, u8* head, u64* size) {
    File file;
    if (File::Open(file, path, File::READ) != File::SUCCESS) {
        return false;
    }
    *size = file.GetSize();
    return *size >= 12 && file.Read(head, 12) >= 0;
}

/// Are the two files the same bytes? Read in pieces of 16 KB with a short rest between them: the card is the game's too.
bool PluginFilesAreEqual(const std::string& a, const std::string& b) {
    File fa;
    File fb;
    if (File::Open(fa, a, File::READ) != File::SUCCESS || File::Open(fb, b, File::READ) != File::SUCCESS) {
        return false;
    }
    const u64 size = fa.GetSize();
    if (size != fb.GetSize()) {
        return false;
    }
    std::vector<u8> ba(16 * 1024), bb(16 * 1024);
    for (u64 done = 0; done < size;) {
        const u32 n = static_cast<u32>(std::min<u64>(ba.size(), size - done));
        if (fa.Read(ba.data(), n) < 0 || fb.Read(bb.data(), n) < 0 || std::memcmp(ba.data(), bb.data(), n) != 0) {
            return false;
        }
        done += n;
        platform::SleepMs(1);
    }
    return true;
}

/// This game's copy (the file Luma loaded: sd:/luma/plugins/<title id>/AzaharLink.3gx) against the plugin of 3DS Link's folder. `detail` says what was found, for the log.
MasterPlugin LookAtMasterPlugin(u64 game_id, std::string* detail) {
    char own_path[96];
    std::snprintf(own_path, sizeof(own_path), "/luma/plugins/%016llX/AzaharLink.3gx", static_cast<unsigned long long>(game_id));
    u8 own_head[12] = {};
    u8 master_head[12] = {};
    u64 own_size = 0;
    u64 master_size = 0;
    if (!ReadPluginHead(own_path, own_head, &own_size)) {
        *detail = std::string("this game's copy is not at ") + own_path + " (the plugin was loaded from another file): nothing to compare";
        return MasterPlugin::Unreadable;
    }
    if (!ReadPluginHead(kMasterPluginPath, master_head, &master_size)) {
        *detail = std::string("the plugin of 3DS Link's folder (") + kMasterPluginPath + ") is not there or cannot be read";
        return MasterPlugin::Unreadable;
    }
    const u32 own = PluginHeaderVersionWord(own_head, 12);
    const u32 master = PluginHeaderVersionWord(master_head, 12);
    const bool same_version = own != 0 && own == master;
    // (the bytes are read only when the two say the same version and have the same size: otherwise they cannot be the same)
    const bool bytes_equal = same_version && own_size == master_size && PluginFilesAreEqual(own_path, kMasterPluginPath);
    const MasterPlugin relation = RelateMaster(own, master, bytes_equal);
    char text[160];
    std::snprintf(text, sizeof(text), "this game's copy: version word 0x%08lx, %llu bytes | 3DS Link's folder: version word 0x%08lx, %llu bytes",
                  static_cast<unsigned long>(own), static_cast<unsigned long long>(own_size), static_cast<unsigned long>(master),
                  static_cast<unsigned long long>(master_size));
    *detail = text;
    return relation;
}

/// Source 2: the title archive (what Checkpoint uses), trying the media this application was
/// launched from first.
std::string ReadIconTitleArchive(u64 title_id, u8 language) {
    // FS:GetMediaType (0x0868): the media the running application came from.
    int first_media = -1;
    {
        u32* cmd = getThreadCommandBuffer();
        cmd[0] = IPC_MakeHeader(0x868, 0, 0);
        Result rc = svcSendSyncRequest(*fsGetSessionHandle());
        if (R_SUCCEEDED(rc)) {
            rc = cmd[1];
        }
        if (R_SUCCEEDED(rc)) {
            first_media = static_cast<int>(cmd[2] & 0xFF);
        }
        Log("game: FS media type of this application: %s (%d)", DescribeResult(rc).c_str(), first_media);
    }
    int medias[4] = {first_media, MEDIATYPE_SD, MEDIATYPE_GAME_CARD, MEDIATYPE_NAND};
    for (int i = 0; i < 4; ++i) {
        const int media = medias[i];
        if (media < 0 || media > 2) {
            continue;
        }
        bool repeated = false;
        for (int j = 0; j < i; ++j) {
            repeated = repeated || medias[j] == media;
        }
        if (repeated) {
            continue;
        }
        u32 archive_path[4] = {static_cast<u32>(title_id), static_cast<u32>(title_id >> 32),
                               static_cast<u32>(media), 0};
        // Content type 0, content index 0, NCCH access type 2 (ExeFS / system menu data), "icon".
        u32 file_path[5] = {0, 0, 2, 0x6E6F6369, 0};
        const FS_Path archive = {PATH_BINARY, sizeof(archive_path), archive_path};
        const FS_Path file = {PATH_BINARY, sizeof(file_path), file_path};
        Handle handle = 0;
        const Result rc = FSUSER_OpenFileDirectly(&handle, ARCHIVE_SAVEDATA_AND_CONTENT, archive,
                                                  file, FS_OPEN_READ, 0);
        char what[24];
        std::snprintf(what, sizeof(what), "title media=%d", media);
        if (R_FAILED(rc)) {
            Log("game: icon[%s] open %s", what, DescribeResult(rc).c_str());
            continue;
        }
        const std::string title = TitleFromSmdh(handle, language, what);
        FSFILE_Close(handle);
        if (!title.empty()) {
            return title;
        }
    }
    return "";
}

/// 3DS Link's title id (DlpLink/tools/common.sh: UNIQUE_ID 0x0F83D1, variation 00: the app 000400000F83D100).
constexpr u64 kAppTitleId = 0x000400000F83D100ull;

/// A PROBE, log only (2026-10-28), made once with the log on, a while after the start: what can the process of a GAME learn of the installed 3DS Link?
///   1. its version, from AM (am:net): the TMD version of the CIA is the version of 3DS Link (1.0.8 = 1032), so a game that sees a later one than its own
///      plugin's could say "open 3DS Link to update your games" even when the app was installed and never opened (the file in /3ds/3DSLink/ is only
///      written by the app when it runs);
///   2. its icon, through the title archive (the way the game's own icon is read: ReadIconTitleArchive): if that is refused, a game cannot read another
///      title's files at all, and the idea of a marker inside the app's content is closed.
/// Nothing is done with the answers: the log says them. (If this stays, it is a diagnostic; if the answers are yes, the warning replaces it.)
void ProbeInstalledApp(const char* plugin_version_text) {
    const Result am_rc = amInit();
    Log("probe: amInit (am:net) from the game's process -> %s", DescribeResult(am_rc).c_str());
    if (R_SUCCEEDED(am_rc)) {
        u64 title_id = kAppTitleId;
        AM_TitleEntry entry = {};
        const Result info_rc = AM_GetTitleInfo(MEDIATYPE_SD, 1, &title_id, &entry);
        if (R_SUCCEEDED(info_rc)) {
            const u32 word = PluginVersionWordFromText(plugin_version_text);
            const u32 own_tmd = (word >> 24) << 10 | ((word >> 16) & 0xFF) << 4 | ((word >> 8) & 0xFF);
            Log("probe: 3DS Link is installed on the SD card: TMD version %u (%u.%u.%u), %llu bytes | this plugin is %s = TMD version %u: %s",
                static_cast<unsigned>(entry.version), static_cast<unsigned>(entry.version >> 10), static_cast<unsigned>((entry.version >> 4) & 0x3F),
                static_cast<unsigned>(entry.version & 0xF), static_cast<unsigned long long>(entry.size), plugin_version_text, static_cast<unsigned>(own_tmd),
                entry.version > own_tmd ? "the app is NEWER than this plugin" : entry.version == own_tmd ? "the same version" : "the app is older");
        } else {
            Log("probe: AM_GetTitleInfo(3DS Link, SD card) -> %s", DescribeResult(info_rc).c_str());
        }
        amExit();
    }
    const std::string title = ReadIconTitleArchive(kAppTitleId, 1);
    Log("probe: the icon of 3DS Link through its title archive: %s%s", title.empty() ? "NOT read" : "read, the title is ", title.c_str());
}

} // namespace

Plugin& Plugin::Get() {
    // Never destroyed: static destructors run while the game is being torn down, with the plugin
    // heap and the framework already half gone, and there is nothing worth cleaning up anyway.
    static Plugin* instance = new Plugin();
    return *instance;
}

// ---------------------------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------------------------

namespace {

/// The file layer of the log on the console: CTRPluginFramework's File.
class CtrFiles : public FileLayer {
public:
    void* OpenAppend(const std::string& path) override {
        File* file = new File();
        if (File::Open(*file, path, File::RWC | File::APPEND) == File::SUCCESS) {
            return file;
        }
        delete file;
        return nullptr;
    }
    u64 Size(void* file) override {
        return static_cast<File*>(file)->GetSize();
    }
    void Write(void* file, const char* data, std::size_t size) override {
        static_cast<File*>(file)->Write(data, static_cast<u32>(size)); // (held by the file system until Flush)
    }
    // The file system buffers writes until a flush, and when the game is killed (Rosalina, power off, a crash) whatever was
    // not flushed is lost: the log layer flushes the main log at every write (its last lines are what a post-mortem needs)
    // and the capture once a pass (until 2026-10-20 at every 16 KB write: a flush costs the card as much as a small write,
    // and the card is the game's too).
    void Flush(void* file) override {
        static_cast<File*>(file)->Flush();
    }
    void Close(void* file) override {
        delete static_cast<File*>(file);
    }
    void Remove(const std::string& path) override {
        File::Remove(path);
    }
    void Rename(const std::string& from, const std::string& to) override {
        File::Rename(from, to);
    }
    bool Exists(const std::string& path) override {
        return File::Exists(path) == 1;
    }
};

} // namespace

bool Plugin::SetDebugLevel(int level) {
    level = level >= 1 ? 1 : 0;
    config_.debug = level;
    SaveConfig(); // (the ini is the one file that is written whatever the level)
    if (level == 0) {
        Log("debug: log OFF: what is left is written, the files are closed, nothing is written from now on");
        log_files_->SetLevel(0, false);
        return true;
    }
    // The capture only has something to capture while the hooks are in.
    const bool ring_ok = log_files_->SetLevel(1, active_);
    Log("debug: log ON: %sAzaharLink.log and AzaharLink.debug.log%s", kDataDir,
        ring_ok ? "" : " (no memory for the capture ring: only the main log is written)");
    if (active_) {
        StartCrumbs();
    }
    return ring_ok;
}

void Plugin::ServiceLog() {
    const u64 now = platform::NowMs();
    if (now - last_log_service_ms_ < kLogServiceIntervalMs) {
        return;
    }
    last_log_service_ms_ = now;
    SampleUdsActivity(now); // (before the window closes: the counters are cleared with it)
    CheckGameMemory(now);
    LogStats(now); // (at every level: the warnings on the screen and the numbers of the menu come from the window)
    if (g_debug_level > 0) {
        if (last_clock_ms_ == 0 || now - last_clock_ms_ >= kClockLineIntervalMs) {
            LogClockLine(last_clock_ms_ == 0 ? "log started" : "every minute");
        }
        log_files_->Flush();
    }
}

// The console's wall clock against the plugin's own time: the "[sss.mmm]" of the main log and the time column of the
// capture are both the time since the plugin started, and the tick behind them stops while the console sleeps. A line
// like this every minute puts a date on any stretch of either, and the uptime next to it shows a sleep (the clock
// keeps running, the uptime does not).
void Plugin::LogClockLine(const char* why) {
    last_clock_ms_ = platform::NowMs();
    char stamp[24];
    FormatConsoleClock(osGetTime(), stamp);
    const unsigned long uptime = static_cast<unsigned long>((last_clock_ms_ - start_ms_) / 1000);
    Log("clock: console clock %s, plugin uptime %lu s (%s)", stamp, uptime, why);
    DebugEvent("clock: console clock %s, plugin uptime %lu s (%s)", stamp, uptime, why);
}

void Plugin::WakeLogWriter() {
    if (log_wake_) {
        svcSignalEvent(log_wake_);
    }
}

void Plugin::StartLogWriter() {
    log_stop_.store(false);
    if (R_FAILED(svcCreateEvent(&log_wake_, RESET_ONESHOT))) {
        log_wake_ = 0; // then it sleeps in slices instead, and a stop takes up to a slice
    }
    // On the game's own core (-1/1 may be unscheduled), above the game's threads (kLogWriterPriority): the SD card write
    // takes tens of milliseconds and must never happen in the game's frame or in the network loop, and what the writer holds
    // must never be waited for by a thread that can preempt it.
    if (!log_thread_.Start(
            [this]() {
                platform::MarkLogWriterThread(); // (the one thread that may wait for the log's locks: azl/platform.hpp)
                bool backlog = false; // the last pass of the capture was cut short (LogFiles::DrainDebug) with records left
                u32 rest_ms = 20;     // ... and how long the card is left alone before the next pass then
                bool capture_early = false; // something asked for the capture to be written at once (the HOME button)
                bool master_checked = false; // the plugin of 3DS Link's folder was compared with this game's copy (once, a while after the start)
                while (!log_stop_.load()) {
                    // Once a second is plenty for the statistics window (all there is to do with the log
                    // off); the debug capture fills a 512 KB ring that wants emptying four times as often,
                    // and a ring that still holds records after a bounded pass is not left for a whole pause.
                    // A stop wakes it at once.
                    const u32 wait_ms = backlog ? rest_ms : g_debug_level > 0 ? 250 : 1000;
                    const u64 asleep = platform::NowTicks();
                    if (log_wake_) {
                        svcWaitSynchronization(log_wake_, static_cast<s64>(wait_ms) * 1000000LL);
                    } else {
                        platform::SleepMs(250);
                    }
                    const u64 woke = platform::NowTicks();
                    // (the lateness of the wake-up: above the game's threads since 2026-10-20 it should be nil;
                    // a late wake-up now means a thread above this one held the core)
                    const u32 slept_ms = static_cast<u32>(azl_ticks_to_ms(woke - asleep));
                    const u32 asked_ms = log_wake_ ? wait_ms : 250;
                    ++g_stats.writer_wakes;
                    if (slept_ms > asked_ms && !log_stop_.load()) {
                        g_stats.writer_late_sum += slept_ms - asked_ms;
                        if (slept_ms - asked_ms > g_stats.writer_late_max) {
                            g_stats.writer_late_max = slept_ms - asked_ms;
                        }
                    }
                    if (!master_checked && platform::NowMs() - start_ms_ >= kMasterCheckAfterMs && !log_stop_.load()) {
                        master_checked = true;
                        std::string detail;
                        const MasterPlugin relation = LookAtMasterPlugin(game_id_, &detail);
                        Log("plugin: %s", detail.c_str());
                        if (g_debug_level > 0) {
                            ProbeInstalledApp(kVersion); // (log only)
                        }
                        if (MasterPluginWarns(relation)) {
                            Log("plugin: 3DS Link keeps %s plugin than this game's copy: open 3DS Link to update your games",
                                relation == MasterPlugin::Newer ? "a newer" : "another build of the same");
                            PostNotification(relation == MasterPlugin::Newer
                                                 ? "AzaharLink: 3DS Link has a newer plugin. Open 3DS Link to update your games."
                                                 : "AzaharLink: this game's plugin is not the one in 3DS Link. Open 3DS Link to update your games.");
                        } else {
                            Log("plugin: this game's copy is %s",
                                relation == MasterPlugin::Same    ? "the plugin of 3DS Link's folder (same bytes)"
                                : relation == MasterPlugin::Older ? "newer than the plugin of 3DS Link's folder (nothing to say)"
                                                                  : "not compared (see above)");
                        }
                    }
                    const u64 card_before = log_files_->card_wait_ticks();
                    ServiceLog();
                    if (g_debug_level > 0) {
                        LogProcessEvents();
                        if (log_flush_requested_.exchange(false)) {
                            log_files_->Flush(); // (the HOME button: see OnHomeButton)
                            capture_early = true;
                        }
                        // The capture goes to the card in few, large passes (LogFileLimits::capture_every_ms): the game reads from the
                        // same card, and a pass every 250 ms kept it waiting (the writer spent 63-77% of its time in the card). A pass
                        // that was cut short comes back soon (backlog); the ring is left to fill meanwhile (it is the plugin's memory).
                        if (backlog || log_files_->CaptureDue(platform::NowMs(), DebugRingUsedBytes(), capture_early)) {
                            capture_early = false;
                            backlog = log_files_->DrainDebug(false);
                        }
                    } else {
                        backlog = false;
                    }
                    // The pass is CPU except for the time its writes spent in the SD card (a wait on the file system's
                    // process, 64% of this thread's wall time in PROVA 2).
                    const u64 pass_ticks = platform::NowTicks() - woke;
                    const u64 card_ticks = log_files_->card_wait_ticks() - card_before;
                    const u64 waited_ticks = card_ticks < pass_ticks ? card_ticks : pass_ticks;
                    StatsAdd64(g_stats.t_writer, pass_ticks - waited_ticks);
                    StatsAdd64(g_stats.t_writer_wait, waited_ticks);
                    // A pass that was cut short comes back soon, but not before the card has had as long as the pass kept
                    // it (20 ms at least, 250 at most): the game reads from the same card, and a capture the card cannot
                    // keep up with (it drops records then, and says so) must not keep the game's loads waiting behind
                    // write after write (2026-10-20).
                    const u32 card_ms = static_cast<u32>(azl_ticks_to_ms(waited_ticks));
                    rest_ms = card_ms < 20 ? 20 : card_ms > 250 ? 250 : card_ms;
                }
            },
            0, kLogWriterPriority)) {
        Log("log: could not start the log writer, lines are only written on events"); // (the statistics window then runs in Tick)
    }
}

// The trail of a sleep (azl/sleep_crumbs.hpp). Its own thread and its own file, at a priority above the game's threads
// (0x20; the writer's is 0x2E): the writer can sit in a long SD write, and what a freeze needs is the line written NOW. It waits on an event with no timeout, except for the seconds
// after a sleep began, when it watches the clock for the wake-up: the rest of the time it costs nothing.
void Plugin::StartCrumbs() {
    if (crumb_wake_ != 0) {
        return; // (once: switching the log off and on again keeps the thread, which writes nothing while the log is off)
    }
    Handle event = 0;
    if (R_FAILED(svcCreateEvent(&event, RESET_ONESHOT))) {
        Log("sleep: could not create the event of the breadcrumb thread: no sleep trail");
        return;
    }
    crumb_stop_.store(false);
    crumb_wake_ = event;
    const auto run = [this]() {
        std::vector<std::string> lines;
        // (the first turn only takes the reference of the clock, before any sleep: the later ones find the sleeps against it)
        crumbs_.Poll(static_cast<u32>(platform::NowMs()), osGetTime(), &lines);
        {
            // The file is created here, with a line of its own, and not by the first event of a sleep: creating a file on the
            // card is the slowest thing the trail does, and a freeze right after the sleep query must find it written.
            char stamp[24];
            FormatConsoleClock(osGetTime(), stamp);
            log_files_->WriteCrumb(std::string("sleep: the trail of this launch begins | tick ") +
                                   std::to_string(static_cast<u32>(platform::NowMs())) + " ms | written at console clock " + stamp);
        }
        while (!crumb_stop_.load()) {
            const s64 wait_ns = crumbs_.watching() ? static_cast<s64>(SleepCrumbs::kPollMs) * 1000000LL : -1;
            svcWaitSynchronization(crumb_wake_, wait_ns);
            if (crumb_stop_.load()) {
                break;
            }
            lines.clear();
            crumbs_.Poll(static_cast<u32>(platform::NowMs()), osGetTime(), &lines);
            for (const std::string& line : lines) {
                log_files_->WriteCrumb(line);
            }
        }
    };
    // 0x20, above the game's threads; 0x2F (the network thread's) if the process may not have that
    int priority = 0;
    for (const int wanted : {0x20, 0x2F}) {
        priority = wanted;
        if (crumb_thread_.Start(run, 0, wanted)) {
            Log("sleep: the breadcrumb thread runs (core %d, priority 0x%x): a sleep leaves its trail in AzaharLink.sleep.log",
                crumb_thread_.core(), priority);
            return;
        }
    }
    crumb_wake_ = 0;
    svcCloseHandle(event);
    Log("sleep: could not start the breadcrumb thread: no sleep trail");
}

// How much memory the game has left. What matters to a game that sizes its heap from what is left
// (Final Fantasy Explorers: 64 MiB minus the commit of the process) is RESLIMIT_COMMIT: a block the
// plugin maps PRIVATE is charged to it, one mapped SHARED is not. The first call, made before the
// game's first instruction, also shows whether the game is promised more than the region still has (a line of the log
// only: nearly every game is promised more than it ever uses, so it says nothing about a problem - what the game really has
// left is CheckGameMemory's).
// The application region at the start of this launch against the last launch of the same title (the user, 2026-10-20: "con
// uscita pulita del plugin ... siamo sicuri al 100% che e' come se non fosse successo niente?"). The kernel frees a process
// whole, so two launches of a title start from the same figure, and the line says so, or by how much they differ. The records
// are kept in AzaharLink.mem, one line per title (the last 16 titles, the most recent last: "title-id-high title-id-low KB-used
// KB-free"), because a launch of the same title right after the last one is rare: the first version kept one record and never
// compared anything in a session of several games. Read and written with the log on only (debug=0 leaves the card alone), on
// the start-up thread, ahead of the game's first instruction.
void Plugin::CompareMemoryWithLastLaunch() {
    if (g_debug_level <= 0) {
        return;
    }
    s64 region = 0;
    s64 used = 0;
    svcGetSystemInfo(&region, 0x10000, 0x80); // Luma: size of the APPLICATION region
    svcGetSystemInfo(&used, 0, 1);            // APPLICATION region in use
    const unsigned long used_kb = static_cast<unsigned long>(used / 1024);
    const unsigned long free_kb = static_cast<unsigned long>((region - used) / 1024);
    const unsigned long hi = static_cast<unsigned long>(game_id_ >> 32);
    const unsigned long lo = static_cast<unsigned long>(game_id_);
    constexpr int kRecords = 16;
    struct Record {
        unsigned long hi, lo, used, free;
    } recs[kRecords + 1];
    int count = 0;
    {
        File file;
        if (File::Open(file, kMemPath, File::READ) == File::SUCCESS) {
            char text[kRecords * 48 + 16] = {};
            const u64 size = file.GetSize();
            if (size > 0 && size < sizeof(text) && file.Read(text, static_cast<u32>(size)) >= 0) {
                const char* at = text;
                while (*at && count < kRecords) {
                    Record r;
                    int chars = 0;
                    if (std::sscanf(at, "%lx %lx %lu %lu%n", &r.hi, &r.lo, &r.used, &r.free, &chars) != 4) {
                        break;
                    }
                    recs[count++] = r;
                    at += chars;
                    while (*at == '\n' || *at == '\r' || *at == ' ') {
                        ++at;
                    }
                }
            }
        }
    }
    int same = -1;
    for (int i = 0; i < count; ++i) {
        if (recs[i].hi == hi && recs[i].lo == lo) {
            same = i;
        }
    }
    if (same >= 0) {
        const long delta = static_cast<long>(used_kb) - static_cast<long>(recs[same].used);
        Log("mem against the last launch of this title: %lu KB used then, %lu KB now (%s%ld KB; free %lu -> %lu KB): %s", recs[same].used,
            used_kb, delta >= 0 ? "+" : "", delta, recs[same].free, free_kb,
            delta == 0                    ? "the same: nothing of the last launch is left"
            : delta > -256 && delta < 256 ? "a few pages apart: the system's bookkeeping, not something left behind"
                                          : "DIFFERENT: worth a look");
        for (int i = same; i + 1 < count; ++i) {
            recs[i] = recs[i + 1]; // (this title moves to the end)
        }
        --count;
    } else {
        Log("mem against the last launch: no record of this title yet (this one is recorded: the next launch of this title is compared with it)");
        if (count == kRecords) {
            for (int i = 0; i + 1 < count; ++i) {
                recs[i] = recs[i + 1]; // (the oldest title goes)
            }
            --count;
        }
    }
    recs[count++] = Record{hi, lo, used_kb, free_kb};
    char text[kRecords * 48 + 16];
    int n = 0;
    for (int i = 0; i < count && n < static_cast<int>(sizeof(text)) - 48; ++i) {
        n += std::snprintf(text + n, sizeof(text) - n, "%08lx %08lx %lu %lu\n", recs[i].hi, recs[i].lo, recs[i].used, recs[i].free);
    }
    MakeDataDir();
    File out;
    if (n > 0 && File::Open(out, kMemPath, File::RWC | File::TRUNCATE | File::SYNC) == File::SUCCESS) {
        out.Write(text, static_cast<u32>(n));
    }
}

void Plugin::LogMemory(const char* when) {
    if (g_debug_level <= 0) {
        return;
    }
    s64 region = 0;
    s64 used = 0;
    svcGetSystemInfo(&region, 0x10000, 0x80); // Luma: size of the APPLICATION region
    svcGetSystemInfo(&used, 0, 1);            // APPLICATION region in use
    const u32 told_to_game = *reinterpret_cast<volatile u32*>(0x1FF80040);

    // Every resource limit of the game's process: current / maximum.
    static const struct {
        ResourceLimitType type;
        const char* name;
    } kLimits[] = {
        {RESLIMIT_COMMIT, "commit KB"},    {RESLIMIT_THREAD, "thread"},
        {RESLIMIT_EVENT, "event"},         {RESLIMIT_MUTEX, "mutex"},
        {RESLIMIT_SEMAPHORE, "semaphore"}, {RESLIMIT_TIMER, "timer"},
        {RESLIMIT_SHAREDMEMORY, "shmem"},  {RESLIMIT_ADDRESSARBITER, "arbiter"},
    };
    constexpr int kCount = sizeof(kLimits) / sizeof(kLimits[0]);
    ResourceLimitType names[kCount];
    s64 cur[kCount];
    s64 max[kCount];
    for (int i = 0; i < kCount; ++i) {
        names[i] = kLimits[i].type;
        cur[i] = max[i] = -1;
    }
    Handle limit = 0;
    if (R_SUCCEEDED(svcGetResourceLimit(&limit, CUR_PROCESS_HANDLE))) {
        svcGetResourceLimitCurrentValues(cur, limit, names, kCount);
        svcGetResourceLimitLimitValues(max, limit, names, kCount);
        svcCloseHandle(limit);
    }

    long long budget = 0;
    long long physical = 0;
    long long over = 0;
    const bool headroom = when && cur[0] >= 0 && max[0] > 0;
    if (headroom) {
        budget = (max[0] - cur[0]) / 1024;
        physical = (region - used) / 1024;
        // Only a real shortfall counts: the two numbers come from different counters and can differ by a
        // few pages (an Old 3DS has region == limit, so there is no slack at all).
        over = budget > physical + 256 ? budget - physical : 0;
    }

    // The game's own heap (0x08000000) and linear memory (0x14000000): size of the mapped region.
    MemInfo heap_info{};
    MemInfo linear_info{};
    PageInfo page;
    svcQueryMemory(&heap_info, &page, 0x08000000);
    svcQueryMemory(&linear_info, &page, 0x14000000);

    const struct mallinfo heap = mallinfo();
    const unsigned long capacity = HeapCapacity();
    char heap_free[48] = "";
    if (capacity) {
        std::snprintf(heap_free, sizeof(heap_free), ", %lu KB free of %lu KB",
                      (capacity > static_cast<unsigned long>(heap.uordblks) ? capacity - heap.uordblks : 0) / 1024,
                      capacity / 1024);
    }
    // The system clock keeps running while the console sleeps and the tick does not: a gap that grows
    // between two of these lines is time spent asleep.
    const long long clock_gap = static_cast<long long>(osGetTime() - clock_base_os_ms_) -
                                static_cast<long long>(platform::NowMs() - clock_base_ms_);
    char wall[24];
    FormatConsoleClock(osGetTime(), wall);
    Log("mem%s%s: game region %ld of %ld KB used (%ld KB free), told to the game %lu KB | plugin heap"
        " %lu KB in use%s, malloc has taken %lu KB | clock gap %lld ms | console clock %s",
        when ? " " : "", when ? when : "", static_cast<long>(used / 1024), static_cast<long>(region / 1024),
        static_cast<long>((region - used) / 1024), static_cast<unsigned long>(told_to_game / 1024),
        static_cast<unsigned long>(heap.uordblks / 1024), capacity ? heap_free : "",
        static_cast<unsigned long>(heap.arena / 1024), clock_gap, wall);
    char line[300];
    int n = std::snprintf(line, sizeof(line), "res%s%s:", when ? " " : "", when ? when : "");
    for (int i = 0; i < kCount && n > 0 && n < static_cast<int>(sizeof(line)) - 40; ++i) {
        const bool kb = i == 0;
        n += std::snprintf(line + n, sizeof(line) - n, " %s %ld/%ld", kLimits[i].name,
                           static_cast<long>(kb ? cur[i] / 1024 : cur[i]),
                           static_cast<long>(kb ? max[i] / 1024 : max[i]));
    }
    // Once the game has made its heap: how far its end is from the socket buffer (never shrinks).
    char gap[80] = "";
    const u32 block = SocketBufferAddress();
    if (block && heap_info.state != MEMSTATE_FREE) {
        std::snprintf(gap, sizeof(gap), ", %ld KB between the heap end and the socket buffer",
                      static_cast<long>((static_cast<long long>(block) - heap_info.base_addr - heap_info.size) / 1024));
    }
    Log("%s | game heap @0x08000000: %lu KB (state 0x%lx), linear @0x14000000: %lu KB (state 0x%lx)%s", line,
        static_cast<unsigned long>(heap_info.size / 1024), static_cast<unsigned long>(heap_info.state),
        static_cast<unsigned long>(linear_info.size / 1024), static_cast<unsigned long>(linear_info.state), gap);
    if (headroom) {
        Log("headroom %s: the game may still commit %lld KB, the region has %lld KB free%s", when, budget, physical,
            over > 0 ? " -> more is promised than is left (the plugin's shared memory is not charged to the game), which"
                       " only matters to a game that fills its whole budget; most never do (Zelda Tri Force Heroes:"
                       " 56,320 KB stay free all session)"
                     : "");
    }
}

// Which parts of the game's address space are in use (state 5 = heap, 11 = locked: stack or the block
// the soc service holds, 4 = code, 3 = static, 6 = shared mapping, 7 = linear): shows where the game's
// SDK put its own mappings relative to the socket buffer.
void Plugin::LogAddressMap() {
    u32 addr = 0x00100000;
    int lines = 0;
    while (addr < 0x14000000 && lines < 40) {
        MemInfo info{};
        PageInfo page;
        if (R_FAILED(svcQueryMemory(&info, &page, addr)) || info.size == 0) {
            break;
        }
        if (info.state != MEMSTATE_FREE) {
            Log("vm: 0x%08lx + %lu KB, state %lu perm %lu", static_cast<unsigned long>(info.base_addr),
                static_cast<unsigned long>(info.size / 1024), static_cast<unsigned long>(info.state),
                static_cast<unsigned long>(info.perm));
            ++lines;
        }
        const u32 next = info.base_addr + info.size;
        if (next <= addr) {
            break;
        }
        addr = next;
    }
}

// The plugin switched itself off at start-up: say so again once a minute (a single message at launch
// is easily missed behind the game's own loading screens).
void Plugin::RepeatStartupIssue() {
    if (active_ || startup_issue_.empty()) {
        return;
    }
    const u64 now = platform::NowMs();
    if (now - last_issue_ms_ >= 60000) {
        last_issue_ms_ = now;
        PostNotification(startup_issue_);
    }
}

void Plugin::LogStats(u64 now_ms) {
    if (last_stats_ms_ == 0) {
        last_stats_ms_ = now_ms;
        (void)TakeStats(); // the first block counts from here, not from the process' start
        uds_idle_.CounterCleared();
        return;
    }
    const u64 dt = now_ms - last_stats_ms_;
    if (dt < kStatsIntervalMs) {
        return;
    }
    last_stats_ms_ = now_ms;
    if (last_mem_ms_ == 0 || now_ms - last_mem_ms_ >= kMemIntervalMs) {
        // Also while the plugin is OFF: tells what a loaded-but-idle plugin costs the game (in the log), and
        // when the plugin's own heap is running out (on the screen, at every level).
        last_mem_ms_ = now_ms;
        if (g_debug_level > 0) {
            LogMemory(nullptr);
        }
        const long free_bytes = HeapFreeBytes();
        if (free_bytes >= 0 && free_bytes < kMinFreeHeapBytes) {
            char text[80];
            std::snprintf(text, sizeof(text), "AzaharLink: plugin memory low (%ld KB free)", free_bytes / 1024);
            Warn(kWarnHeap, text);
        }
    }

    const Stats s = TakeStats();
    uds_idle_.CounterCleared(); // (the counter started again: a look at it now is a look at a new window)

    // What the window says goes on the screen at every level: the numbers of the menu's status page and the
    // warnings. Only the lines below are the log's.
    const RoomClient::LinkStats link = room_->link_stats();
    NoteStatusNumbers(s, dt, link);
    CheckStatsWarnings(s, now_ms);
    CheckDebugLoad(s);
    CheckLinkSilence(s);
    if (g_debug_level <= 0) {
        return;
    }

    u32 rx = 0, tx = 0;
    for (int i = 0; i < 6; ++i) {
        rx += s.rx_frames[i];
        tx += s.tx_frames[i];
    }
    static unsigned quiet = 0;
    // (a link lost or a player gone is worth its line even when nothing else was going on)
    const bool eventful = s.net_stalls || s.loss_deferred || s.loss_resumed || s.loss_declared || s.transport_down[0] ||
                          s.transport_down[1] || s.transport_down[2] || s.presence_host_gone || s.node_removed[3] ||
                          s.node_removed[4] || s.hle_declined;
    if (rx == 0 && tx == 0 && s.ipc_requests == 0 && !eventful) {
        // Nothing going on (menus, no session): one line per minute is enough to show we are alive.
        if (++quiet % 12 != 1) {
            return;
        }
    } else {
        quiet = 0;
    }

    const unsigned long secs = static_cast<unsigned long>(dt / 1000);
    Log("stats %lus: room rx %lu frames (bcn %lu data %lu auth %lu assoc %lu deauth %lu map %lu) %lu B"
        " | tx %lu (bcn %lu data %lu auth %lu assoc %lu deauth %lu map %lu) %lu B, %lu reliable %lu unreliable",
        secs, static_cast<unsigned long>(rx), static_cast<unsigned long>(s.rx_frames[0]),
        static_cast<unsigned long>(s.rx_frames[1]), static_cast<unsigned long>(s.rx_frames[2]),
        static_cast<unsigned long>(s.rx_frames[3]), static_cast<unsigned long>(s.rx_frames[4]),
        static_cast<unsigned long>(s.rx_frames[5]), static_cast<unsigned long>(s.rx_bytes),
        static_cast<unsigned long>(tx), static_cast<unsigned long>(s.tx_frames[0]),
        static_cast<unsigned long>(s.tx_frames[1]), static_cast<unsigned long>(s.tx_frames[2]),
        static_cast<unsigned long>(s.tx_frames[3]), static_cast<unsigned long>(s.tx_frames[4]),
        static_cast<unsigned long>(s.tx_frames[5]), static_cast<unsigned long>(s.tx_bytes),
        static_cast<unsigned long>(s.tx_reliable), static_cast<unsigned long>(s.tx_unreliable));
    Log("stats: link rtt %lu+-%lu ms, loss %lu/1000, resent %lu, in flight %lu B, throttle %lu, rtt samples skipped %lu |"
        " queue max %lu frames %lu B, dropped: stale %lu cap %lu throttled %lu | copies sent %lu, delivered early %lu | send wait avg %lu max %lu ms | net loop:"
        " %lu rounds (%lu without a wait, %lu with a 1 ms wait), longest pause %lu ms, longest round %lu ms, %lu socket errors",
        static_cast<unsigned long>(link.rtt_ms), static_cast<unsigned long>(link.rtt_var_ms),
        static_cast<unsigned long>(link.loss_permille), static_cast<unsigned long>(link.lost_total),
        static_cast<unsigned long>(link.in_transit), static_cast<unsigned long>(link.throttle),
        static_cast<unsigned long>(s.ack_samples_skipped),
        static_cast<unsigned long>(s.queue_max), static_cast<unsigned long>(s.tx_queue_bytes_max),
        static_cast<unsigned long>(s.tx_stale_drops), static_cast<unsigned long>(s.tx_cap_drops),
        static_cast<unsigned long>(s.tx_throttled), static_cast<unsigned long>(s.tx_duplicates), static_cast<unsigned long>(s.rx_early),
        static_cast<unsigned long>(s.delay_n ? s.delay_sum / s.delay_n : 0),
        static_cast<unsigned long>(s.delay_max), static_cast<unsigned long>(s.loop_iters),
        static_cast<unsigned long>(s.polls_now), static_cast<unsigned long>(s.polls_quick),
        static_cast<unsigned long>(s.loop_gap_max), static_cast<unsigned long>(s.service_max),
        static_cast<unsigned long>(s.service_errors));
    Log("stats: uds delivered %lu, routed %lu | dropped: no-bind %lu src-filter %lu overwritten %lu"
        " not-for-me %lu echo %lu not-connected %lu mgmt %lu | pull %lu (empty %lu too-big %lu,"
        " ring peak %lu units) | sendto %lu (%lu errors) %lu B | ipc %lu avg %lu us max %lu us |"
        " host beacons %lu, longest silence %lu ms",
        static_cast<unsigned long>(s.data_delivered), static_cast<unsigned long>(s.forwarded),
        static_cast<unsigned long>(s.drop_no_bind), static_cast<unsigned long>(s.drop_src_filter),
        static_cast<unsigned long>(s.ring_overwritten), static_cast<unsigned long>(s.drop_not_for_me),
        static_cast<unsigned long>(s.drop_echo), static_cast<unsigned long>(s.drop_not_connected),
        static_cast<unsigned long>(s.drop_management), static_cast<unsigned long>(s.pull_calls),
        static_cast<unsigned long>(s.pull_empty), static_cast<unsigned long>(s.pull_too_large),
        static_cast<unsigned long>(s.ring_peak_units), static_cast<unsigned long>(s.sendto_calls),
        static_cast<unsigned long>(s.sendto_errors), static_cast<unsigned long>(s.sendto_bytes),
        static_cast<unsigned long>(s.ipc_requests),
        static_cast<unsigned long>(s.ipc_requests ? s.ipc_sum_ticks / s.ipc_requests / 268 : 0),
        static_cast<unsigned long>(s.ipc_max_ticks / 268), static_cast<unsigned long>(s.host_beacons),
        static_cast<unsigned long>(s.host_beacon_age_max));
    {
        u32 pulls = 0;
        for (const u32 n : s.pull_latency) {
            pulls += n;
        }
        if (pulls > 0) {
            // (from the ring's stamps: how long each frame the game took had waited for its PullPacket)
            Log("stats: queued -> PullPacket: <1 ms %lu, <2 %lu, <5 %lu, <10 %lu, <20 %lu, <50 %lu, <100 %lu, more %lu; longest %lu ms",
                static_cast<unsigned long>(s.pull_latency[0]), static_cast<unsigned long>(s.pull_latency[1]),
                static_cast<unsigned long>(s.pull_latency[2]), static_cast<unsigned long>(s.pull_latency[3]),
                static_cast<unsigned long>(s.pull_latency[4]), static_cast<unsigned long>(s.pull_latency[5]),
                static_cast<unsigned long>(s.pull_latency[6]), static_cast<unsigned long>(s.pull_latency[7]),
                static_cast<unsigned long>(s.pull_latency_max));
        }
    }
    LogStatsTiming(s, dt);
    LogStatsWire(s);
    LogStatsCapture();

    // Command counts, once in a while.
    static unsigned rounds = 0;
    if (++rounds % 6 == 0) {
        char line[200];
        int n = std::snprintf(line, sizeof(line), "stats: uds commands in the last 30 s:");
        for (int i = 0; i < kStatsCommands && n > 0 && n < static_cast<int>(sizeof(line)) - 16; ++i) {
            if (g_cmd_counts[i]) {
                n += std::snprintf(line + n, sizeof(line) - n, " 0x%02x=%lu", i,
                                   static_cast<unsigned long>(g_cmd_counts[i]));
            }
        }
        std::memset(g_cmd_counts, 0, sizeof(g_cmd_counts));
        Log("%s", line);
    }
}

void Plugin::NoteStatusNumbers(const Stats& s, u64 dt_ms, const RoomClient::LinkStats& link) {
    const StatusNumbers numbers = MakeStatusNumbers(s, dt_ms, link.rtt_ms, link.loss_permille, g_rcvbuf_got);
    platform::LockGuard lock(status_mutex_);
    status_numbers_ = numbers;
}

namespace {

/// snprintf onto the end of a line that is built piece by piece (stops at the end of the buffer).
void Append(char* text, std::size_t size, int* n, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
void Append(char* text, std::size_t size, int* n, const char* fmt, ...) {
    if (*n < 0 || static_cast<std::size_t>(*n) >= size) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    const int added = std::vsnprintf(text + *n, size - *n, fmt, args);
    va_end(args);
    if (added > 0) {
        *n += added;
    }
}

} // namespace

// Where the plugin's time goes per second and what the game does meanwhile, to tell a slow plugin from a slow game from a
// slow link. The figures that say "CPU" are CPU: the time each thread spent in our code less what it waited for inside it
// (a call to the soc process, the engine's lock, the SD card, its own pauses; see Stats), so nothing is counted twice and a
// thread blocked for a whole second adds nothing. They are upper bounds: a thread that another one preempted inside our
// code is charged for the time. The waits are in a group of their own, and the network loop's wall time is split into
// its CPU, its waits and what is left, to show that the pieces add up.
void Plugin::LogStatsTiming(const Stats& s, u64 dt_ms) {
    const CpuNumbers c = MakeCpuNumbers(s, dt_ms);
    const unsigned long fps10 = static_cast<unsigned long>(static_cast<u64>(s.frames) * 10000 / (dt_ms ? dt_ms : 1));
    const unsigned long gap_avg10 = s.frames ? static_cast<unsigned long>(s.frame_gap_sum / s.frames / (AZL_TICKS_PER_MS / 10)) : 0;
    const unsigned long lock_max10 = static_cast<unsigned long>(static_cast<u64>(s.lock_wait_max) * 10 / AZL_TICKS_PER_MS);
    const unsigned long hold_max_us = static_cast<unsigned long>(static_cast<u64>(s.lock_hold_max) / 268);
    const unsigned long hold_avg_us = static_cast<unsigned long>(s.lock_holds ? s.lock_hold_ticks / s.lock_holds / 268 : 0);
    const unsigned long net_waiting = static_cast<unsigned long>(c.soc10) + c.poll10 + c.pause10 + c.netlock10;
    char text[1100];
    int n = 0;
    Append(text, sizeof(text), &n,
           "stats: ms per second, CPU time of our code (waiting is not counted) | plugin total %lu.%lu (CPU) = %lu.%lu%% of a core",
           static_cast<unsigned long>(c.plugin10) / 10, static_cast<unsigned long>(c.plugin10) % 10,
           static_cast<unsigned long>(c.plugin10) / 100, static_cast<unsigned long>(c.plugin10) / 10 % 10);
    Append(text, sizeof(text), &n,
           " | net thread %lu.%lu: enet %lu.%lu, frames+engine %lu.%lu, queue+tick %lu.%lu",
           static_cast<unsigned long>(c.net10) / 10, static_cast<unsigned long>(c.net10) % 10,
           static_cast<unsigned long>(c.enet10) / 10, static_cast<unsigned long>(c.enet10) % 10,
           static_cast<unsigned long>(c.frames10) / 10, static_cast<unsigned long>(c.frames10) % 10,
           static_cast<unsigned long>(c.queue10) / 10, static_cast<unsigned long>(c.queue10) % 10);
    Append(text, sizeof(text), &n, " | game threads: uds %lu.%lu (%lu calls), %lu other ipc calls, frame callback %lu.%lu",
           static_cast<unsigned long>(c.uds10) / 10, static_cast<unsigned long>(c.uds10) % 10,
           static_cast<unsigned long>(s.ipc_requests), static_cast<unsigned long>(s.pass_calls),
           static_cast<unsigned long>(c.frame10) / 10, static_cast<unsigned long>(c.frame10) % 10);
    Append(text, sizeof(text), &n, " | log writer %lu.%lu wall, not CPU, not in the total (%lu wake-ups, late avg %lu max %lu ms)",
           static_cast<unsigned long>(c.writer10) / 10, static_cast<unsigned long>(c.writer10) % 10,
           static_cast<unsigned long>(s.writer_wakes),
           static_cast<unsigned long>(s.writer_wakes ? s.writer_late_sum / s.writer_wakes : 0),
           static_cast<unsigned long>(s.writer_late_max));
    Append(text, sizeof(text), &n,
           " | waiting, not CPU: soc recv+send %lu.%lu, poll %lu.%lu (waiting), SD card %lu.%lu, pauses after socket errors %lu.%lu",
           static_cast<unsigned long>(c.soc10) / 10, static_cast<unsigned long>(c.soc10) % 10,
           static_cast<unsigned long>(c.poll10) / 10, static_cast<unsigned long>(c.poll10) % 10,
           static_cast<unsigned long>(c.sd10) / 10, static_cast<unsigned long>(c.sd10) % 10,
           static_cast<unsigned long>(c.pause10) / 10, static_cast<unsigned long>(c.pause10) % 10);
    Append(text, sizeof(text), &n, " | engine lock: %lu waits %lu.%lu ms total, longest %lu.%lu ms; held %lu times, avg %lu us, longest %lu us by the %s",
           static_cast<unsigned long>(s.lock_contended), static_cast<unsigned long>(c.lock10) / 10,
           static_cast<unsigned long>(c.lock10) % 10, lock_max10 / 10, lock_max10 % 10, static_cast<unsigned long>(s.lock_holds),
           hold_avg_us, hold_max_us, s.lock_hold_max_net ? "net thread" : "game");
    Append(text, sizeof(text), &n, " | net loop %lu.%lu = cpu %lu.%lu + waiting %lu.%lu + other %lu.%lu",
           static_cast<unsigned long>(c.wall10) / 10, static_cast<unsigned long>(c.wall10) % 10,
           static_cast<unsigned long>(c.net10) / 10, static_cast<unsigned long>(c.net10) % 10, net_waiting / 10,
           net_waiting % 10, static_cast<unsigned long>(c.other10) / 10, static_cast<unsigned long>(c.other10) % 10);
    Append(text, sizeof(text), &n,
           " | game: %lu frames (%lu.%lu fps), interval avg %lu.%lu max %lu ms, %lu longer than 50 ms | net rounds longer"
           " than 50 ms: %lu",
           static_cast<unsigned long>(s.frames), fps10 / 10, fps10 % 10, gap_avg10 / 10, gap_avg10 % 10,
           static_cast<unsigned long>(s.frame_gap_max / AZL_TICKS_PER_MS), static_cast<unsigned long>(s.frame_hitches),
           static_cast<unsigned long>(s.net_slow_rounds));
    log_files_->Append(text);
}

// The wire: the soc calls by kind, what one service call drained (= how full the soc ring got), the gaps
// between the datagrams of the room (its holds), the twin frames, and what the engine did with nodes.
void Plugin::LogStatsWire(const Stats& s) {
    static const char* const kSocNames[kSocKinds] = {"recv", "recv-empty", "send", "poll"};
    char text[1600];
    int n = 0;
    Append(text, sizeof(text), &n, "stats: soc calls n (mean/max us, >20 ms, >100 ms):");
    for (int k = 0; k < kSocKinds; ++k) {
        Append(text, sizeof(text), &n, " %s %lu (%lu/%lu, %lu, %lu)", kSocNames[k],
               static_cast<unsigned long>(s.soc_calls[k]),
               static_cast<unsigned long>(s.soc_calls[k] ? s.soc_ticks[k] / s.soc_calls[k] / 268 : 0),
               static_cast<unsigned long>(s.soc_max_ticks[k] / 268), static_cast<unsigned long>(s.soc_over20[k]),
               static_cast<unsigned long>(s.soc_over100[k]));
    }
    // The ring counts payload + 32 bytes per datagram; what one service call found in it is how full it got.
    const unsigned long ring = g_rcvbuf_got > 0 ? static_cast<unsigned long>(g_rcvbuf_got) : 4416;
    const unsigned long fill = s.drain_bytes_max + 32ul * s.drain_dgrams_max;
    Append(text, sizeof(text), &n,
           " | one service call read at most %lu datagrams %lu B (soc ring about %lu%% of %lu B), slowest call %lu ms",
           static_cast<unsigned long>(s.drain_dgrams_max), static_cast<unsigned long>(s.drain_bytes_max),
           fill * 100 / ring, ring, static_cast<unsigned long>(s.service_max_ticks / AZL_TICKS_PER_MS));
    Append(text, sizeof(text), &n,
           " | gaps between room datagrams (of %lu): 50-99 ms %lu, 100-149 %lu, 150-249 %lu, 250-499 %lu, 500+ %lu",
           static_cast<unsigned long>(s.rx_datagrams), static_cast<unsigned long>(s.gap_hist[0]),
           static_cast<unsigned long>(s.gap_hist[1]), static_cast<unsigned long>(s.gap_hist[2]),
           static_cast<unsigned long>(s.gap_hist[3]), static_cast<unsigned long>(s.gap_hist[4]));
    // The host's second copies (an Azahar host re-broadcasts what it relays): frames that came through the host
    // and could have a twin, those dropped because the direct twin was delivered, those delivered as the only copy.
    // The room's reliable stream (ENet): packets that arrived behind a hole and the longest wait behind one, resent copies of
    // packets we had already handled, the datagrams we sent that held only acknowledgements (and those where the acknowledgement
    // rode on data), and how long a read pass was (datagrams, ms): the three causes of a gap told apart.
    Append(text, sizeof(text), &n,
           " | enet room stream: out of order %lu (waited up to %lu ms), resent copies received %lu, ack-only datagrams sent %lu"
           " (acks riding on data %lu), read passes %lu (longest %lu datagrams, %lu ms, %lu cut short), empty receives skipped %lu,"
           " packets handed over before their ack %lu (ack limit hit %lu times, longest ack hold %lu ms), link-back pings %lu,"
           " SendTo with the broadcast flag addressed to a node's MAC %lu",
           static_cast<unsigned long>(s.enet_ooo), static_cast<unsigned long>(s.enet_hol_max_ms),
           static_cast<unsigned long>(s.enet_dup_rx), static_cast<unsigned long>(s.enet_ack_only),
           static_cast<unsigned long>(s.enet_ack_data), static_cast<unsigned long>(s.enet_passes),
           static_cast<unsigned long>(s.enet_pass_dgrams_max), static_cast<unsigned long>(s.enet_pass_ms_max),
           static_cast<unsigned long>(s.enet_passes_capped), static_cast<unsigned long>(s.enet_recv_skipped),
           static_cast<unsigned long>(s.enet_events_before_ack), static_cast<unsigned long>(s.enet_ack_limit_hits),
           static_cast<unsigned long>(s.enet_ack_hold_max_ms), static_cast<unsigned long>(s.enet_silence_probes),
           static_cast<unsigned long>(s.sendto_addressed));
    Append(text, sizeof(text), &n, " | copies dropped %lu | twin candidates %lu (dropped %lu, delivered alone %lu)",
           static_cast<unsigned long>(s.drop_dup_copy), static_cast<unsigned long>(s.fwd_candidates), static_cast<unsigned long>(s.drop_fwd_dup),
           static_cast<unsigned long>(s.fwd_unmatched));
    if (s.fwd_evictions || s.drop_bad_size || s.ring_flushed || s.binds_signalled) {
        Append(text, sizeof(text), &n,
               " | twin table full %lu, empty or oversize frames %lu, queued frames dropped at a new session %lu,"
               " bind events at a session end %lu",
               static_cast<unsigned long>(s.fwd_evictions), static_cast<unsigned long>(s.drop_bad_size),
               static_cast<unsigned long>(s.ring_flushed), static_cast<unsigned long>(s.binds_signalled));
    }
    if (s.deauth_rx[0] || s.deauth_rx[1] || s.deauth_rx[2] || s.node_removed[0] || s.node_removed[1] ||
        s.node_removed[2] || s.connect_result[0] || s.connect_result[1] || s.connect_result[2]) {
        Append(text, sizeof(text), &n,
               " | deauth rx host/client/connecting %lu/%lu/%lu, nodes removed by deauth/new auth/eject %lu/%lu/%lu,"
               " connects ok/host silent/cut short %lu/%lu/%lu",
               static_cast<unsigned long>(s.deauth_rx[0]), static_cast<unsigned long>(s.deauth_rx[1]),
               static_cast<unsigned long>(s.deauth_rx[2]), static_cast<unsigned long>(s.node_removed[0]),
               static_cast<unsigned long>(s.node_removed[1]), static_cast<unsigned long>(s.node_removed[2]),
               static_cast<unsigned long>(s.connect_result[0]), static_cast<unsigned long>(s.connect_result[1]),
               static_cast<unsigned long>(s.connect_result[2]));
    }
    if (s.node_removed[3] || s.node_removed[4] || s.transport_down[0] || s.transport_down[1] || s.transport_down[2] ||
        s.presence_host_gone || s.net_stalls || s.loss_deferred || s.loss_resumed || s.loss_declared) {
        // The room and the game's session: players who left the room, links that ended, and whether a pause of
        // our own network thread was taken for one.
        Append(text, sizeof(text), &n,
               " | room: nodes removed (left the room %lu, link lost %lu), host left the room %lu, session ended by a lost"
               " link (client %lu, connect %lu, host %lu), losses declared %lu / waited for %lu / session kept %lu,"
               " network thread away 2 s or more %lu times",
               static_cast<unsigned long>(s.node_removed[3]), static_cast<unsigned long>(s.node_removed[4]),
               static_cast<unsigned long>(s.presence_host_gone), static_cast<unsigned long>(s.transport_down[0]),
               static_cast<unsigned long>(s.transport_down[1]), static_cast<unsigned long>(s.transport_down[2]),
               static_cast<unsigned long>(s.loss_declared), static_cast<unsigned long>(s.loss_deferred),
               static_cast<unsigned long>(s.loss_resumed), static_cast<unsigned long>(s.net_stalls));
    }
    if (s.drop_foreign || s.foreign_shadow || s.deauth_late || s.hle_throttled) {
        // The filters of the link: another lobby of the room (dropped by a host; counted, and dropped only when the client's
        // filter is on), the second copy of a goodbye that came after the node had joined again, and a game thread that
        // polls a failing command (its failing replies waited half a millisecond each).
        Append(text, sizeof(text), &n,
               " | filters: frames of another lobby in the room %lu dropped / %lu passed on by a client that only counts,"
               " late second Deauthentications ignored %lu, failing replies of a polling game thread delayed %lu",
               static_cast<unsigned long>(s.drop_foreign), static_cast<unsigned long>(s.foreign_shadow),
               static_cast<unsigned long>(s.deauth_late), static_cast<unsigned long>(s.hle_throttled));
    }
    if (s.scans || s.sync_checks) {
        // Scan and beacon-sync evidence (both only counted so far): do the game's wlancommid / id8 equal
        // its host's, and does the host's node list agree with what the Logoff frames said?
        Append(text, sizeof(text), &n,
               " | scans %lu (%lu networks, %lu with another wlancommid/id8 than asked, %lu odd inputs), beacon sync:"
               " %lu checks, %lu differ, longest run %lu",
               static_cast<unsigned long>(s.scans), static_cast<unsigned long>(s.scan_entries),
               static_cast<unsigned long>(s.scan_id_differs), static_cast<unsigned long>(s.scan_odd_input),
               static_cast<unsigned long>(s.sync_checks), static_cast<unsigned long>(s.sync_disagree),
               static_cast<unsigned long>(s.sync_streak_max));
    }
    log_files_->Append(text);
}

// The capture's own window (a line of its own: the lines above keep their shape for the scripts that read them). What
// went into the ring in the window by lane, what did not and why (no room / the lock was busy for longer than a
// producer waits), how many IPC calls the diet counted instead of writing, and how full the ring got.
void Plugin::LogStatsCapture() {
    if (!DebugRingReady()) {
        return;
    }
    const CaptureTotals now = DebugCaptureTotals();
    const CaptureTotals& before = capture_prev_;
    const unsigned long queued = DebugBytesQueued();
    Log("stats: capture: ring %lu KB now, peak %lu KB (of 512, the last 32 for the control lane) | stored: control %lu,"
        " heavy %lu | lost: heavy %lu (ring full %lu, lock busy %lu), control %lu (%lu, %lu), markers written %lu |"
        " the diet: %lu IPC calls in %lu lines",
        queued / 1024, static_cast<unsigned long>(now.ring_peak / 1024),
        static_cast<unsigned long>(now.records[0] - before.records[0]),
        static_cast<unsigned long>(now.records[1] - before.records[1]),
        static_cast<unsigned long>(now.lost_full[1] + now.lost_busy[1] - before.lost_full[1] - before.lost_busy[1]),
        static_cast<unsigned long>(now.lost_full[1] - before.lost_full[1]),
        static_cast<unsigned long>(now.lost_busy[1] - before.lost_busy[1]),
        static_cast<unsigned long>(now.lost_full[0] + now.lost_busy[0] - before.lost_full[0] - before.lost_busy[0]),
        static_cast<unsigned long>(now.lost_full[0] - before.lost_full[0]),
        static_cast<unsigned long>(now.lost_busy[0] - before.lost_busy[0]),
        static_cast<unsigned long>(now.markers - before.markers),
        static_cast<unsigned long>(now.agg_calls - before.agg_calls),
        static_cast<unsigned long>(now.agg_lines - before.agg_lines));
    capture_prev_ = now;
}

// What the counters of the last interval say is wrong goes on screen too (once a minute per kind): the user
// does not read logs while playing.
void Plugin::CheckStatsWarnings(const Stats& s, u64 now_ms) {
    char text[96];
    const u32 tx_drops = s.tx_stale_drops + s.tx_cap_drops;
    if (tx_drops > 20) {
        std::snprintf(text, sizeof(text), "AzaharLink: %lu frames not sent in %lu s (slow link?)",
                      static_cast<unsigned long>(tx_drops), static_cast<unsigned long>(kStatsIntervalMs / 1000));
        Warn(kWarnTxDrops, text);
    }
    if (s.hle_declined) {
        // The game got the real wireless service: its Enter would drop the WiFi that carries the room.
        Warn(kWarnHle, "AzaharLink: out of sessions, the game uses the real wireless");
    }
    if (s.ring_overwritten > frameslost::kWarnAbove) {
        // Two things look the same here: the game is a slow reader, or it does not read at all (paused, loading, the lid): its
        // UDS calls tell them apart (azl/screen_text.hpp).
        const u32 idle_ms = uds_idle_.IdleMs(now_ms);
        const bool not_reading = frameslost::NotReading(s.ipc_requests, idle_ms);
        if (Warn(kWarnOverflow, frameslost::Text(s.ring_overwritten, not_reading))) {
            // (what the text was chosen from, in the log next to the line that says it was shown)
            Log("frames lost: %lu in the window; the game made %lu UDS calls in it (%lu PullPackets), the last one %lu ms ago:"
                " %s",
                static_cast<unsigned long>(s.ring_overwritten), static_cast<unsigned long>(s.ipc_requests),
                static_cast<unsigned long>(s.pull_calls), static_cast<unsigned long>(idle_ms),
                not_reading ? "it is not reading the network" : "it reads, too slowly");
        }
    }
}

// With the log on, a notice when it is the log that slows the game (the user, 2026-10-20: the debug mode may show more on the
// screen than the production one): the writer's time in the SD card over the window, and the share of the capture's heavy
// records the ring dropped. Once a minute at most (Warn), and in the log as every warning.
void Plugin::CheckDebugLoad(const Stats& s) {
    if (g_debug_level <= 0 || !DebugRingReady()) {
        return;
    }
    const u32 card_pct = static_cast<u32>(azl_ticks_to_ms(s.t_writer_wait) * 100 / kStatsIntervalMs);
    const CaptureTotals now = DebugCaptureTotals();
    const CaptureTotals& before = capture_prev_; // (LogStatsCapture, later in the same window, moves it on)
    const u64 lost = (now.lost_full[1] + now.lost_busy[1]) - (before.lost_full[1] + before.lost_busy[1]);
    const u64 offered = (now.records[1] - before.records[1]) + lost;
    const u32 lost_pct = offered ? static_cast<u32>(lost * 100 / offered) : 0;
    if (card_pct >= 50 || lost_pct >= 30) {
        char text[96];
        std::snprintf(text, sizeof(text), "AzaharLink debug: SD card busy %lu%%, capture lost %lu%% (the log slows the game)",
                      static_cast<unsigned long>(card_pct), static_cast<unsigned long>(lost_pct));
        Warn(kWarnDebugLoad, text);
    }
}

// With the log on: the room's datagrams stopped for 150 ms or more. The socket sees it before anything of the plugin's does, so it is
// the WiFi link (or the router), not the game and not this plugin: in the session of 2026-10-20 one console had 17-23 of these a
// minute in a Tekken fight and the other 0-2, the same room and the same plugin, while the plugin's own pauses stayed under 70 ms.
// A silence stops a lockstep game on both consoles for as long as it lasts, and each one costs the pair three or four stalls.
void Plugin::CheckLinkSilence(const Stats& s) {
    if (g_debug_level <= 0) {
        return;
    }
    const u32 silent = s.gap_hist[2] + s.gap_hist[3] + s.gap_hist[4]; // 150-249, 250-499 and 500+ ms
    if (s.rx_datagrams >= 100 && silent >= 2) {
        char text[96];
        std::snprintf(text, sizeof(text), "AzaharLink debug: room link silent %lu times (150+ ms): WiFi", static_cast<unsigned long>(silent));
        Warn(kWarnSilence, text);
    }
}

void Plugin::SampleUdsActivity(u64 now_ms) {
    uds_idle_.Look(__atomic_load_n(&g_stats.ipc_requests, __ATOMIC_RELAXED), now_ms);
}

// The game's memory (azl/screen_text.hpp): what the application region has left, read about 20 s after the start - a game
// is done allocating by then - and every 30 s. The first reading is the one toast of the start ("New 3DS memory mode: 55 MB
// free", whatever the figure: a game that takes its whole region at once is normal); a later reading that FELL below the limit
// says so ("Game memory almost full: 3 MB free"), once each time it falls.
void Plugin::CheckGameMemory(u64 now_ms) {
    if (!active_) {
        return; // (the plugin is off for this launch: it has nothing to say about a game it is not in)
    }
    const u32 uptime = static_cast<u32>(now_ms - start_ms_);
    if (!mem_watch_.Due(uptime)) {
        return;
    }
    s64 region = 0;
    s64 used = 0;
    svcGetSystemInfo(&region, 0x10000, 0x80); // Luma: size of the APPLICATION region
    svcGetSystemInfo(&used, 0, 1);            // APPLICATION region in use
    const gamemem::Reading reading = gamemem::MakeReading(region, used);
    {
        platform::LockGuard lock(status_mutex_);
        mem_reading_ = reading;
    }
    switch (mem_watch_.Update(reading, uptime)) {
    case gamemem::Watch::Say::Info: {
        const std::string text = gamemem::InfoText(reading);
        Log("memory: %s (region %ld KB, %ld KB used) - shown on screen", text.c_str(), reading.region_kb, reading.used_kb);
        PostNotification(text);
        break;
    }
    case gamemem::Watch::Say::Low: {
        const std::string text = gamemem::LowText(reading);
        Log("WARNING (shown on screen): %s (region %ld KB, %ld KB used, the limit is %ld KB)", text.c_str(), reading.region_kb,
            reading.used_kb, gamemem::kLowKb);
        PostNotification(text);
        break;
    }
    case gamemem::Watch::Say::Nothing:
        break;
    }
}

bool Plugin::Warn(int kind, const std::string& message) {
    const u64 now = platform::NowMs();
    if (warned_ms_[kind] != 0 && now - warned_ms_[kind] < kWarnIntervalMs) {
        return false;
    }
    warned_ms_[kind] = now;
    Log("WARNING (shown on screen): %s", message.c_str());
    PostNotification(message);
    return true;
}

long Plugin::HeapFreeBytes() const {
    const unsigned long capacity = HeapCapacity();
    if (!capacity) {
        return -1;
    }
    const unsigned long used = static_cast<unsigned long>(mallinfo().uordblks);
    return used < capacity ? static_cast<long>(capacity - used) : 0;
}

void Plugin::NoteProcessEvent(int event) {
    // (the framework's keep-alive thread: flag stores only)
    if (event >= 0 && event < kProcessEvents) {
        event_ms_[event] = static_cast<u32>(platform::NowMs());
        event_count_[event] = event_count_[event] + 1;
        if (event == kEventSleepEnter || event == kEventSleepExit) {
            // (the trail of the sleep: a few stores and one event signal)
            NoteSleepEvent(event == kEventSleepEnter ? SleepCrumbs::kFwEnter : SleepCrumbs::kFwExit, 0);
        }
    }
}

void Plugin::LogProcessEvents() {
    static const char* const kNames[kProcessEvents] = {"EXIT",       "SLEEP_ENTER", "SLEEP_EXIT", "HOME_ENTER",
                                                       "HOME_EXIT", "SWAP_ENTER",  "SWAP_EXIT"};
    if (g_debug_level <= 0) {
        return;
    }
    using CTRPluginFramework::Process;
    static_assert(static_cast<int>(Process::Event::EXIT) == kEventExit &&
                      static_cast<int>(Process::Event::SLEEP_ENTER) == kEventSleepEnter &&
                      static_cast<int>(Process::Event::SLEEP_EXIT) == kEventSleepExit,
                  "the indices of the framework's process events");
    for (int i = 0; i < kProcessEvents; ++i) { // (EXIT too: the line that says the game was closing, before 'shutdown: begin')
        const u32 count = event_count_[i];
        // (the writer thread and Shutdown() both come here: whoever swaps the count in reports it, the other does not)
        if (event_reported_[i].exchange(count) == count) {
            continue;
        }
        const u32 since_start = event_ms_[i] - static_cast<u32>(start_ms_);
        // The console's clock when the line is written: within a writer wake (250 ms) of the event.
        char wall[24];
        FormatConsoleClock(osGetTime(), wall);
        Log("process event %s (#%lu) at %lu.%03lu s: the game %s | console clock %s", kNames[i],
            static_cast<unsigned long>(count), static_cast<unsigned long>(since_start / 1000),
            static_cast<unsigned long>(since_start % 1000),
            i == kEventExit ? "is closing" : i <= 2 ? "sleeps (lid) or wakes" : "goes to / returns from the HOME menu",
            wall);
    }
}

void Plugin::SampleLinkState(u64 now_ms) {
    if (now_ms < next_link_sample_ms_) {
        return;
    }
    next_link_sample_ms_ = now_ms + kLinkSampleIntervalMs;
    const u8 strength = *reinterpret_cast<volatile const u8*>(kSharedPageWifiStrength);
    const u8 state = *reinterpret_cast<volatile const u8*>(kSharedPageWifiState);
    if (link_known_ && strength == link_strength_ && state == link_state_) {
        return;
    }
    if (!link_known_) {
        Log("link: shared page wifi strength %u, network state %u (first sample; room: %s)", strength, state,
            RoomClient::StateName(room_->state()));
    } else if (link_changes_ < 200) {
        Log("link: shared page wifi strength %u -> %u, network state %u -> %u (room: %s)", link_strength_, strength,
            link_state_, state, RoomClient::StateName(room_->state()));
    }
    ++link_changes_;
    link_known_ = true;
    link_strength_ = strength;
    link_state_ = state;
}

void Plugin::CheckSocketBuffer() {
    if (g_rcvbuf_asked <= 0 || !azl_rcvbuf_degraded(g_rcvbuf_asked, g_rcvbuf_got)) {
        return; // as asked (or unreadable: the log line of the socket layer says so)
    }
    char text[96];
    std::snprintf(text, sizeof(text), "AzaharLink: socket buffer is %d KB, %d KB asked (frames may be lost)",
                  g_rcvbuf_got / 1024, g_rcvbuf_asked / 1024);
    Warn(kWarnSocket, text);
}

// ---------------------------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------------------------

void Plugin::EarlyInit() {
    if (initialized_) {
        return;
    }
    initialized_ = true;
    start_ms_ = platform::NowMs();
    file_layer_.reset(new CtrFiles());
    MakeDataDir(); // (the framework opens the SD card before PatchProcess: the folder can be made here)
    bootdiag::Begin(); // (the diagnostic build only: the state of the process before we touch anything, on the card at once)
    log_files_.reset(new LogFiles(file_layer_.get(), kDataDir, start_ms_));
    log_files_->SetConsoleClock(&ConsoleClockMs); // (the first line of every capture file)
    SetLogSink([this](const char* line) { log_files_->Append(line); });

    // The settings come first. Two of them have to be known BEFORE the game runs and before the first line is
    // logged: whether the plugin is on at all (the hooks cannot be taken out later without breaking sessions the
    // game already holds: it opens its ndm:u session in the first seconds) and whether anything is logged
    // (debug=0: Log() returns at once, no file is created and no ring is allocated). The framework opens the SD card
    // before calling PatchProcess. If the settings cannot be read, the plugin stays ON and the log stays off.
    const bool config_read_early = LoadConfig();
    active_ = config_.enabled && config_.tunnel_enabled;
    bool capture_ready = true;
    if (config_.debug > 0) {
        g_debug_level = 1;
        // The capture must be ready before the first hooked IPC call of the game (it has nothing to capture while
        // the plugin is off).
        capture_ready = !active_ || DebugInit();
    }
    if (g_debug_level > 0) {
        // (the console's own clock, to line the log up with the PC's and with the dates of the files on the card:
        // the [sss.mmm] of the lines is only the time since the plugin started)
        char stamp[24];
        FormatConsoleClock(osGetTime(), stamp);
        Log("==== AzaharLink %s (built %s), console clock %s ====", kVersion, kBuildStamp, stamp);
    }
    if (!capture_ready) {
        Log("debug: not enough memory for the capture ring: only AzaharLink.log is written");
    }

    std::memcpy(console_mac_.data(), reinterpret_cast<const void*>(kSharedPageWifiMac), 6);

    CTRPluginFramework::Process::GetName(process_name_);
    game_id_ = CTRPluginFramework::Process::GetTitleID();
    if (process_name_.empty()) {
        process_name_ = "3DS game";
    }
    game_name_ = process_name_;
    Log("game: %s (title id %08lx%08lx), console MAC %s", process_name_.c_str(),
        static_cast<unsigned long>(game_id_ >> 32), static_cast<unsigned long>(game_id_),
        MacToString(console_mac_).c_str());
    // Which build this is, in a way the log can show: libctru's hidInit() opens ir:rst (C-stick, ZL, ZR) on a New 3DS unless
    // the program says otherwise (Sources/main.cpp), and a plugin that holds it leaves the game without its C-stick.
    {
        const bool irrst = hidShouldUseIrrst();
        Log("hid: the plugin %s ir:rst (C-stick and ZL/ZR %s)", irrst ? "OPENS" : "does not open",
            irrst ? "are taken from the game" : "stay with the game");
    }
    // Which family of console this is, from the size of the application memory region (64 MiB on an
    // Old 3DS / 2DS, 124 MiB on a New 3DS). Nothing below depends on it: the socket buffer is placed
    // from the limits the kernel reports for this very process.
    {
        s64 region = 0;
        svcGetSystemInfo(&region, 0x10000, 0x80);
        const bool new_layout = region >= (100ll << 20);
        Log("console: application memory region %lld MiB (%s memory layout)", region >> 20,
            new_layout ? "New 3DS" : "Old 3DS");
        // The plugin's own memory: the block Luma loaded it into (3gx MemorySize), shared with the game's
        // mapping so that it is not charged to the game's commit. Luma takes it from the APPLICATION region of
        // a New 3DS and from the SYSTEM region of an Old 3DS.
        const CTRPluginFramework::PluginHeader* header = CTRPluginFramework::FwkSettings::Header;
        if (header) {
            Log("plugin block: image %lu KB + heap %lu KB (0x%08lx), malloc may use %lu KB (heap size 0x%lx less 0x%lx"
                " kept by the framework), taken from the %s region",
                static_cast<unsigned long>(header->exeSize / 1024), static_cast<unsigned long>(header->heapSize / 1024),
                static_cast<unsigned long>(header->heapVA), HeapCapacity() / 1024,
                static_cast<unsigned long>(header->heapSize), kFrameworkReserved,
                new_layout ? "APPLICATION" : "SYSTEM");
        } else {
            Log("plugin block: the framework's header is not available");
        }
    }
    clock_base_os_ms_ = osGetTime();
    clock_base_ms_ = start_ms_;
    // Before the game's own start-up (this runs ahead of its first instruction): what the plugin
    // block has already cost the game's budget.
    LogMemory("at start");
    CompareMemoryWithLastLaunch();

    Log("config: read at startup: %s, plugin %s", config_read_early ? "yes" : "no (defaults)",
        active_ ? "ON" : "OFF (enabled=0 or tunnel=0): no hooks, no network");
    if (config_from_old_place_) {
        Log("config: read from the old place %s (there is no %s yet: the 3DS Link app moves the file when it runs)", kOldConfigPath,
            kConfigPath);
    }
    // The receive ring of the room socket: 64 KB, which the ini key rcvbuf_kb can only lower (more makes the
    // soc service fail or crash, see azl/rcvbuf.h; the socket layer caps it again where it is used).
    const char* settings_issue = nullptr; // a value of the settings that was not taken (shown if nothing worse is)
    if (config_.rcvbuf_kb != ini::kRcvbufKbDefault) {
        const int asked = azl_rcvbuf_from_kb(config_.rcvbuf_kb);
        if (asked > 0) {
            azl_enet_rcvbuf_bytes = azl_rcvbuf_limit(asked, 0);
            Log("config: rcvbuf_kb=%d: the receive ring is %d KB", config_.rcvbuf_kb, azl_enet_rcvbuf_bytes / 1024);
        } else {
            Log("config: rcvbuf_kb=%d ignored (8, 16, 32 or 64 are allowed): the receive ring stays 64 KB",
                config_.rcvbuf_kb);
            settings_issue = "rcvbuf_kb is not 8, 16, 32 or 64: ignored";
        }
    }
    if (active_) {
        // The hooks give the game a wireless that leads into the room through sockets: without
        // memory for them the plugin stays out of the game (the real wireless keeps working).
        const bool reserved = !bootdiag::Skip("soc") && ReserveSocketMemory();
        LogMemory(reserved ? "with the socket buffer" : "without a socket buffer");
        if (!reserved) {
            active_ = false;
            startup_issue_ = "No memory for the network: AzaharLink is OFF for this launch";
            Log("plugin OFF for this launch: no memory for the network");
        } else {
            startup_issue_ = socplan::StartupIssue(SocketBufferSize(), g_debug_level > 0);
        }
        if (startup_issue_.empty() && settings_issue) {
            startup_issue_ = settings_issue;
        }
        if (!startup_issue_.empty()) {
            PostNotification(startup_issue_);
        }
    }

    room_.reset(new RoomClient());

    UdsEngine::Transport transport;
    transport.send = [this](WifiPacket& packet) {
        if (!room_->joined()) {
            return; // not in a room: frames go nowhere, like an emulator without a connection
        }
        packet.transmitter_address = room_->mac();
        room_->SendWifiPacket(packet);
    };
    transport.get_mac = [this]() { return room_->joined() ? room_->mac() : console_mac_; };
    engine_.reset(new UdsEngine(&kernel_, transport));
    hle_.reset(new UdsHle(engine_.get()));
    hle_->SetDlpProbeFake(AsksDlpOnlyForTheWireless(game_id_));
    if (DlpModeOf(game_id_) == DlpGameMode::Lobby && active_) {
        // The Download Play lobby of the room, for the games that play over it (Kirby Triple Deluxe, Planet Robobot): the one new
        // allocation of the lobby; the other games never have one. It is made here, after the engine and the HLE and before the
        // hooks (below) and before the room is started (Connect, long after), and neither the pointer nor the hub is changed
        // from here on: the network thread's tick callback and the game's threads read them without a lock.
        dlp_hub_ = MakeDlpHub(engine_.get(), &kernel_, game_id_, console_mac_);
        if (dlp_hub_) {
            hle_->SetDlpHub(dlp_hub_.get());
            const u64 seed = Fnv1a64(console_mac_.data(), console_mac_.size());
            Log("dlp: the Download Play lobby is carried by the room for this game (title id %08lx%08lx): dlp:FKCL and dlp:SRVR are"
                " served by the plugin, friend code seed %08lx%08lx, %lu bytes of hub",
                static_cast<unsigned long>(game_id_ >> 32), static_cast<unsigned long>(game_id_),
                static_cast<unsigned long>(seed >> 32), static_cast<unsigned long>(seed), static_cast<unsigned long>(sizeof(DlpHub)));
        } else {
            Log("dlp: no memory for the Download Play lobby: dlp:FKCL and dlp:SRVR stay the real services for this game");
        }
    }

    room_->SetWifiCallback([this](const WifiPacket& packet) { engine_->OnWifiPacket(packet); });
    room_->SetWifiPrefilter([this](const u8* message, std::size_t size) { return engine_->TryDiscardEncoded(message, size); });
    // (after the engine's own round: the hub serves the request a game thread left and pumps the lobby, on this thread - the one
    // thread that may call the lobby's network side. dlp_hub_ is set above, before the room exists as a thread, and never changed.)
    room_->SetTickCallback([this](u64 now_ms) {
        engine_->Tick(now_ms);
        if (dlp_hub_) {
            dlp_hub_->Tick();
        }
    });
    room_->SetRoomInfoCallback([this](const room::RoomInfo& info) { OnRoomInfo(info); });
    room_->SetStateCallback([this](RoomClient::State state, RoomClient::Error error) {
        Log("room: %s%s%s", RoomClient::StateName(state),
            error != RoomClient::Error::None ? " - " : "",
            error != RoomClient::Error::None ? RoomClient::ErrorName(error) : "");
        if (state == RoomClient::State::Connecting) {
            engine_->ResetRoomPresence(); // (the lists of the room before this attempt say nothing about the next join)
        }
        if (state == RoomClient::State::Connecting || state == RoomClient::State::Idle) {
            platform::LockGuard lock(members_mutex_);
            room_members_.clear(); // (the next RoomInfo is the room as it is, not a crowd that just came in)
            room_members_known_ = false;
        }
        if (state == RoomClient::State::Joining) {
            CheckSocketBuffer(); // the socket of this attempt exists: did the soc service give the ring we asked for?
        }
        const bool left_joined = state != RoomClient::State::Joined && joined_ms_ != 0;
        if (state == RoomClient::State::Joined) {
            joined_ms_ = platform::NowMs();
            joined_os_ms_ = osGetTime();
        } else if (joined_ms_ != 0) {
            // How long the link lasted, how it was doing, and whether the system clock kept running while the
            // tick did not (the console slept): the numbers a lost link or a lid close is told apart by.
            const RoomClient::LinkStats link = room_->link_stats();
            const long long clock_gap = static_cast<long long>(osGetTime() - joined_os_ms_) -
                                        static_cast<long long>(platform::NowMs() - joined_ms_);
            Log("room: the link lasted %lu s (Joined -> %s): last ENet rtt %lu+-%lu ms, loss %lu/1000, %lu B in"
                " flight, clock gap (system clock - tick) %lld ms",
                static_cast<unsigned long>((platform::NowMs() - joined_ms_) / 1000), RoomClient::StateName(state),
                static_cast<unsigned long>(link.rtt_ms), static_cast<unsigned long>(link.rtt_var_ms),
                static_cast<unsigned long>(link.loss_permille), static_cast<unsigned long>(link.in_transit), clock_gap);
            joined_ms_ = 0;
        }
        // What the end of the link means for the game's session (and what the screen says about it).
        const bool said = left_joined && OnRoomLeft(error);
        if (state == RoomClient::State::Idle && error != RoomClient::Error::None && !left_joined) {
            // An attempt to join that did not get there: which one, why, and the errno of the socket call that failed
            // (the next one is the policy's: 2 s later, or when the WiFi is back - see RejoinPolicy).
            const int err = room_->last_errno();
            char errno_text[24] = "";
            if (err != 0) {
                std::snprintf(errno_text, sizeof(errno_text), ", errno %d", err);
            }
            Log("room: attempt %lu to join failed: %s%s", static_cast<unsigned long>(rejoin_.attempts()),
                RoomClient::ErrorName(error), errno_text);
        }
        if (state == RoomClient::State::Joined) {
            const room::RoomInfo info = room_->room_info();
            std::string members;
            for (const auto& member : info.members) {
                members += (members.empty() ? "" : ", ") + member.nickname + " [" + member.game_name + "]";
            }
            Log("room: \"%s\" has %u/%u players: %s", info.name.c_str(),
                static_cast<unsigned>(info.members.size()), static_cast<unsigned>(info.member_slots),
                members.c_str());
            last_notified_error_ = -1;
            if (!OnRoomJoined()) {
                PostNotification("Joined room \"" + info.name + "\" (" +
                                 std::to_string(info.members.size()) + " players)");
            }
            // The version that runs, on the line below the first join of the launch (the log has it too: it is what one looks at to know
            // which build a console really ran after an update).
            static bool version_said = false;
            if (!version_said) {
                version_said = true;
                PostNotification(std::string("AzaharLink ") + kVersion + " (built " + kBuildStamp + ")");
            }
        } else if (state == RoomClient::State::Idle && error != RoomClient::Error::None &&
                   static_cast<int>(error) != last_notified_error_) {
            // Once per distinct error: a reconnect loop must not cover the game with overlays.
            last_notified_error_ = static_cast<int>(error);
            if (!said) {
                PostNotification(std::string("Room: ") + RoomClient::ErrorName(error));
            }
        }
        if (g_debug_level > 0) {
            // State changes are what a post-mortem needs most: do not keep them in memory. The writer thread puts them on
            // the card within a moment; this thread - the network thread, or the main one - never waits for the SD card
            // (the lock of the file was held through whole 16 KB writes, 0.3 to 32 s: PROVA 1).
            log_flush_requested_.store(true);
            WakeLogWriter();
        }
    });

    // The HLE is ON from the very first request. The game opens its ndm:u session during the first
    // seconds, before main() runs (0.1.2 log: "game opened ndm:u" at boot). If that session were
    // the real one, the game's EnterExclusiveState(LocalCommunications) later makes the system drop
    // the infrastructure WiFi and our room connection dies (0.1.3 regression).
    if (active_) {
        // The game's APT session tells when the console goes to sleep; we only look (see OnSleepEnter).
        SetSleepObserver([](bool enter) {
            if (enter) {
                Plugin::Get().OnSleepEnter();
            } else {
                Plugin::Get().OnWake();
            }
        });
        SetHomeObserver([]() { Plugin::Get().OnHomeButton(); });
        SetAptObserver(&Plugin::NoteSleepEvent); // (the trail of a sleep: with the log on)
        if (!bootdiag::Skip("hooks")) {
            hook_stats_ = InstallSvcHooks(hle_.get());
        }
    }
#ifdef AZL_BOOT_DIAG
    // The diagnostic build: what EarlyInit logged goes to the card now (the writer thread starts ~5 s from now, and a game that dies before leaves nothing),
    // the state of the process after our changes, and a watch of ten seconds.
    if (g_debug_level > 0) {
        log_files_->Open();
        log_files_->FlushWithin(500);
    }
    bootdiag::Mark("after the plugin's start-up (socket buffer and hooks done)");
    bootdiag::StartWatch();
#endif
}

void Plugin::ResolveIdentity() {
    // The name set on the console (Settings > Other settings > Profile), as UTF-16 in the config
    // block 0x000A0000. The game normally has access to the config service; if not, Luma can lend
    // a session (see the srvGetServiceHandle wrapper in soc.cpp).
    u8 language = 1;
    if (R_SUCCEEDED(cfguInit())) {
        u16 name[0x1C / 2 + 1] = {};
        const Result rc = CFGU_GetConfigInfoBlk2(0x1C, 0x000A0000, name);
        CFGU_GetSystemLanguage(&language);
        cfguExit();
        if (R_SUCCEEDED(rc)) {
            std::string ascii;
            for (int i = 0; i < 10 && name[i]; ++i) {
                ascii += name[i] < 0x80 ? static_cast<char>(name[i]) : '_';
            }
            console_name_ = SanitizeNickname(ascii);
            if (dlp_hub_) {
                // The name every list of the Download Play lobby shows for this player (Kirby Triple Deluxe and Planet Robobot send their
                // Initialize with no name): the console's own, kNameUnits UTF-16 units as the config block keeps them.
                dlp_hub_->SetPlayerName(name);
            }
        } else {
            Log("identity: cannot read the console's user name (0x%08lx)", static_cast<unsigned long>(rc));
        }
    } else {
        Log("identity: config service not available, the nickname will be generic");
    }
    if (console_name_.empty()) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "3DS %02X%02X", console_mac_[4], console_mac_[5]);
        console_name_ = buf;
    }
    language_ = language;
}

void Plugin::ResolveGameName() {
    if (!config_.game_name.empty()) {
        platform::LockGuard lock(name_mutex_);
        game_name_ = config_.game_name;
        name_source_ = NameSource::Config;
        Log("game: name set in the configuration: '%s'", game_name_.c_str());
        return;
    }
    if (R_FAILED(fsInit())) {
        Log("game: fsInit failed, cannot read the game's icon");
    } else {
        std::string title = ReadIconSelfNcch(language_);
        const char* source = "own icon (self NCCH)";
        if (title.empty()) {
            title = ReadIconTitleArchive(game_id_, language_);
            source = "title archive icon";
        }
        fsExit();
        if (!title.empty()) {
            platform::LockGuard lock(name_mutex_);
            Log("game: name '%s' from the %s (the process name was '%s')", title.c_str(), source,
                process_name_.c_str());
            game_name_ = title;
            name_source_ = NameSource::Icon;
            return;
        }
    }
    Log("game: no icon could be read: using '%s' until another player in the room tells us the name",
        process_name_.c_str());
}

void Plugin::OnRoomInfo(const room::RoomInfo& info) {
    // Runs on the network thread. If somebody else in the room plays the same game (same title id)
    // they already carry its proper name (Azahar sends the English title from the game's icon):
    // take it when we only know the 8-character process name.
    if (shutting_down_.load()) {
        return;
    }
    NoteRoomMembers(info);
    {
        // Who is in the room and plays: a player that leaves it takes its node out of the game's session (the engine waits
        // a moment before it believes it, and nothing goes on the wire). So does one that stays in the room but tells it
        // that it plays nothing (azl/room_members.hpp: an emulator that is closed says no goodbye on the air, and the host
        // waited 30 s for its frames, Kirby Battle Royale 2026-10-28).
        std::vector<Mac> members;
        members.reserve(info.members.size());
        for (const room::MemberInfo& member : info.members) {
            if (RoomMemberPlays(member.game_name, member.game_id)) {
                members.push_back(member.mac);
            }
        }
        engine_->OnRoomMembers(members);
    }
    if (game_id_ == 0) {
        return;
    }
    for (const room::MemberInfo& member : info.members) {
        if (member.nickname == JoinedNickname() || member.game_id != game_id_ ||
            member.game_name.empty()) {
            continue;
        }
        const std::string name = SanitizeTitle(member.game_name);
        if (name.empty() || name == process_name_) {
            continue; // nothing to learn: another console that only knows the process name
        }
        {
            platform::LockGuard lock(name_mutex_);
            if (name_source_ != NameSource::ProcessName) {
                if (name != game_name_) {
                    static bool said = false;
                    if (!said) {
                        said = true;
                        Log("game: '%s' says our game is called '%s', keeping '%s'",
                            member.nickname.c_str(), name.c_str(), game_name_.c_str());
                    }
                }
                return;
            }
            game_name_ = name;
            name_source_ = NameSource::RoomPeer;
        }
        Log("game: name '%s' learned from '%s' in the room", name.c_str(), member.nickname.c_str());
        room_->UpdateGameInfo(name, game_id_);
        return;
    }
}

// A line for every player that came into the room or left it, with the MAC the game's frames will be told apart by (the
// room lists everybody again at every change; the first list of a join is the baseline, and the "has N/M players" line
// of the join says who was there).
void Plugin::NoteRoomMembers(const room::RoomInfo& info) {
    platform::LockGuard lock(members_mutex_);
    std::vector<RoomMemberRow> now;
    now.reserve(info.members.size());
    for (const room::MemberInfo& member : info.members) {
        now.push_back({member.mac, member.nickname, SanitizeTitle(member.game_name), RoomMemberPlays(member.game_name, member.game_id)});
    }
    if (room_members_known_ && g_debug_level > 0) {
        // (azl/room_members.hpp: "room: 'Friend-A' [FINAL FANTASY EXPLORERS] joined the room ..."; a player the room has not
        // yet told the game of has no brackets)
        for (const std::string& line : RoomMemberLines(room_members_, now, info.member_slots)) {
            Log("%s", line.c_str());
        }
        for (const std::string& line : RoomMemberStoppedLines(room_members_, now)) {
            Log("%s", line.c_str()); // (a player still in the room that plays nothing any more: the engine counts it out)
        }
    }
    room_members_ = std::move(now);
    room_members_known_ = true;
}

// ---------------------------------------------------------------------------------------------
// The room link and the game's session
// ---------------------------------------------------------------------------------------------

void Plugin::Announce(const std::string& text) {
    const std::string now_text = announcer_.Offer(text, platform::NowMs());
    if (!now_text.empty()) {
        PostNotification(now_text);
    }
}

bool Plugin::DropSession(RoomClient::Error error) {
    const bool dropped = engine_->OnTransportDown(NetworkStatusChangeReason::ConnectionLost);
    if (dropped) {
        StatsAdd(g_stats.loss_declared); // (the network thread and the main thread both get here)
        session_dropped_.store(true);
    }
    Log("room: the link is gone (%s): %s", error == RoomClient::Error::None ? "stopped here" : RoomClient::ErrorName(error),
        dropped ? "the game's session is over" : "the game had no session to end");
    if (error == RoomClient::Error::RoomClosed) {
        Announce("Room closed by its host");
        return true;
    }
    if (dropped && error == RoomClient::Error::LostConnection) {
        Announce("Room link lost: game disconnected");
        return true;
    }
    return false;
}

bool Plugin::OnRoomLeft(RoomClient::Error error) {
    engine_->ResetRoomPresence();
    if (shutting_down_.load()) {
        return false; // the game is closing: its session goes with it
    }
    const u64 now = platform::NowMs();
    if (error != RoomClient::Error::None) {
        rejoin_.NoteLoss(now); // (the room lists a member whose link died without a goodbye for up to 30 s)
    }
    const u32 stall = room_->RecentStallMs(now, LossGuard::kLookBackMs);
    if (guard_.OnLeft(error, stall, room_->mac(), JoinedNickname(), now) == LossGuard::Verdict::Drop) {
        return DropSession(error);
    }
    // ENet (and the room) gave up on us after our own network thread had been silent for a long time: the link may be
    // perfectly fine, so the game's session waits a while for the room to take this member back.
    StatsAdd(g_stats.loss_deferred);
    Log("room: the link ended after the network thread had been away for %lu ms: the game's session waits up to %lu s"
        " for the room to take us back",
        static_cast<unsigned long>(stall), static_cast<unsigned long>(LossGuard::kRejoinMs / 1000));
    return false;
}

bool Plugin::OnRoomJoined() {
    rejoin_.NoteJoined();
    switch (guard_.OnJoined(room_->mac(), JoinedNickname(), platform::NowMs())) {
    case LossGuard::Outcome::Kept:
        StatsAdd(g_stats.loss_resumed);
        Log("room: back in the room as the same member: the game's session goes on");
        break;
    case LossGuard::Outcome::Drop:
        DropSession(RoomClient::Error::LostConnection);
        break;
    case LossGuard::Outcome::Nothing:
        break;
    }
    if (!session_dropped_.exchange(false)) {
        return false;
    }
    Announce("Back in the room: reopen the lobby");
    return true;
}

// The observers of the game's APT session run on the game's own thread, inside its call (see hook.cpp): no
// printf family, no Log() with a format, no buffer over 64 bytes (Tests/policy_check.sh). What the sleep does beyond
// its two lines (the engine's StopService, the room's RequestStop) is the HLE proper.
// >>> game-thread code (Tests/policy_check.sh)
bool Plugin::SleepActs() const {
    // 1 acts for a console that is not hosting, 2 forces it for a host too (azl/apt_sleep.hpp: SleepActsFor)
    const int mode = config_.sleep_stop;
    return SleepActsFor(mode, mode == 1 && engine_->status() == NetworkStatus::ConnectedAsHost);
}

void Plugin::NoteSleepEvent(u32 kind, u32 value) {
    Plugin& self = Get();
    if (g_debug_level <= 0 || self.crumb_wake_ == 0) {
        return; // (no log, no trail)
    }
    self.crumbs_.Push(kind, value, static_cast<u32>(platform::NowMs()));
    svcSignalEvent(self.crumb_wake_);
}

void Plugin::OnSleepEnter() {
    // The game's thread, inside the APT call that told it: the sleep is certain from here (a refused one never gets
    // this far), so the session ends now, before the game acknowledges and the console goes under. With
    // sleep_stop off (the default) the sleep is only logged and the session is left to the room's timeouts.
    const bool acts = SleepActs();
    sleep_.Enter(static_cast<u32>(platform::NowMs()), acts);
    if (!acts) {
        LogLine line;
        if (config_.sleep_stop == 0) {
            line.Str("sleep: the console goes to sleep (APT notification 5): sleep_stop is off, nothing is done");
        } else {
            line.Str("sleep: the console goes to sleep (APT notification 5): this console hosts and sleep_stop=1 acts for a"
                     " client only (2 forces it for a host too): nothing is done");
        }
        return;
    }
    {
        LogLine line;
        line.Str("sleep: the console goes to sleep (APT notification 5): the game's session and the room link end,"
                 " like a console's");
    }
    rejoin_.NoteLoss(platform::NowMs());
    if (engine_->StopService(kServiceSleep)) {
        session_dropped_.store(true); // (a session was up)
    }
    room_->RequestStop(true); // (the network thread sends what is queued, a Deauthentication among it, and leaves)
}

void Plugin::OnWake() {
    sleep_.Woke(); // (the main thread acts on it: Tick)
}

void Plugin::OnHomeButton() {
    log_flush_requested_.store(true);
    WakeLogWriter(); // (one svc: an event signal)
}
// <<< game-thread code

void Plugin::HandleWake(u64 now_ms) {
    // (`acts` is only looked at when the framework reported a wake-up: with sleep_stop=1 working it out takes the engine's
    // lock, which this frame callback has no business with on every frame)
    const bool acts = sleep_.NeedsActs(event_count_[kEventSleepExit]) && SleepActs();
    const SleepWatch::Result r = sleep_.Update(static_cast<u32>(now_ms), event_count_[kEventSleepEnter],
                                               event_count_[kEventSleepExit], acts);
    if (r.apt_wake) {
        Log("sleep: the console woke up (APT notification 6)");
    }
    if (r.fw_wake) {
        Log("sleep: the framework reports that the console woke up");
    }
    if (r.missed) {
        // The game's APT calls never showed us this sleep (it may not ask the system, or not through a call we see):
        // the link under the session is stale all the same.
        Log("sleep: the framework saw a sleep that the game's APT calls did not show: the session ends now");
        rejoin_.NoteLoss(now_ms);
        if (engine_->OnTransportDown(NetworkStatusChangeReason::RequestFromSystem)) {
            session_dropped_.store(true);
        }
    }
    if (r.timed_out) {
        Log("sleep: no wake-up was seen %lu s after the sleep notification: going on",
            static_cast<unsigned long>(SleepWatch::kWakeWaitMs / 1000));
    }
    if (r.woke) {
        rejoin_.ResetBackoff();
        Announce("Waking up: reconnecting to the room");
        if (g_debug_level > 0) {
            LogClockLine("woke up"); // (the tick stopped in the sleep: the clock says how long it was)
        }
    }
}

void Plugin::LateInit() {
    if (late_initialized_) {
        return;
    }
    late_initialized_ = true;

    // Open the log files and write what was recorded before the file system was usable. With debug=0 this does
    // nothing at all: no file is created.
    log_files_->Open();
    StartLogWriter();

    const bool loaded = LoadConfig();
    if (config_.debug > 0 && g_debug_level == 0) {
        // The settings could not be read at start-up but say "log on" now.
        log_files_->SetLevel(1, active_);
    }
    config_.debug = g_debug_level; // (the menu label and the toggle follow what the log really does)
    if (g_debug_level > 0 && active_) {
        StartCrumbs(); // (the trail of a sleep: only with the log on, and only while the hooks are in)
    }
    // The settings that act on the lower layers.
    platform::SetPreferredCore(config_.net_core);
    azl_enet_socket_buffers = config_.socket_buffers ? 1 : 0;
    room_->SetFastData(config_.radio_like);
    Log("config: %s enabled=%d host='%s' port=%u nickname='%s' tunnel=%d autoconnect=%d radio_like=%d"
        " socket_buffers=%d rcvbuf_kb=%d sleep_stop=%d net_thread_core=%d menu_key=%s",
        loaded ? "loaded" : "no file,", config_.enabled, config_.host.c_str(), config_.port,
        config_.nickname.c_str(), config_.tunnel_enabled, config_.autoconnect, config_.radio_like,
        config_.socket_buffers, config_.rcvbuf_kb, config_.sleep_stop, config_.net_core, config_.menu_key.c_str());
    Log("debug: log ON (%s)", !active_ ? "AzaharLink.log only: no capture while the plugin is OFF"
                              : DebugRingReady() ? "AzaharLink.log and AzaharLink.debug.log"
                                                 : "AzaharLink.log only: no memory for the capture ring");

    if (!active_) {
        Log("plugin is OFF for this launch (the game uses the real local wireless)");
        return;
    }
    if (!config_.enabled || !config_.tunnel_enabled) {
        // The settings could not be read at startup but say "off" now: the hooks are in and the
        // game may already hold fake sessions, so it stays ON until the next launch.
        Log("WARNING: the settings say OFF but could not be read at startup: staying ON for this launch");
    }

    // The framework installed its own hooks in the first seconds: check that none of them landed on
    // one of our patched sites.
    ReportForeignPatches();

    ResolveIdentity();
    ResolveGameName();
    if (g_debug_level > 0) {
        LogAddressMap();
    }
    Log("identity: nickname '%s' (%s), MAC %s, game name '%s'", EffectiveNickname().c_str(),
        config_.nickname.empty() ? "from the console" : "from the settings",
        MacToString(console_mac_).c_str(), GameName().c_str());

    if (hook_stats_.send_sync_sites == 0) {
        Log("WARNING: no svc 0x32 stub found in the game's code: UDS cannot be intercepted");
        PostNotification("AzaharLink: cannot hook this game (no system call found in its code): multiplayer will not work");
    }
}

void Plugin::RequestStop() {
    // Called from the framework's exit event on a very small stack: flag stores only.
    shutting_down_.store(true);
    BeginTeardown();
    if (room_) {
        room_->RequestStop(true);
    }
    log_stop_.store(true);
    WakeLogWriter(); // (one svc: fine on this stack)
    crumb_stop_.store(true);
    if (crumb_wake_) {
        svcSignalEvent(crumb_wake_);
    }
}

void Plugin::Shutdown() {
    if (!initialized_ || shut_down_) {
        return;
    }
    shut_down_ = true;
    shutting_down_.store(true);
    // The lines nobody has reported yet - the EXIT event above all: the writer reports it too if it wakes first, and
    // whichever of the two comes first logs it once - come before 'shutdown: begin', so that the end of a log reads in
    // the order it happened.
    LogProcessEvents();
    Log("shutdown: begin (%lu hook sites, network %s)",
        static_cast<unsigned long>(hook_stats_.send_sync_sites),
        room_ && room_->state() != RoomClient::State::Idle ? "up" : "idle");
    // From here the capture has half a second to empty its ring (a full ring on a slow card takes tens of seconds, the writer is
    // then stuck in the card's writes and the main log's last lines - how the exit went - never reach the file, as on the full
    // card of the Zelda session: log_files.hpp, LogFileLimits::exit_capture_ms).
    log_files_->BeginShutdown();
    // The hooks stay in the game's code on purpose: the game's own threads may still be running
    // through them while it closes, rewriting live code without invalidating the instruction cache
    // can crash them, and the trampolines stay mapped until the process dies anyway. The HLE keeps
    // answering the fake sessions the game still holds.

    // The Download Play hub (a game in Lobby mode) before the network thread: the thread that serves its requests is about to go.
    // Stop() answers a request still in the queue with the failure and makes every later one fail at once, so no game thread is
    // left waiting for a network thread that is not there. The hub itself is NOT destroyed, here or ever (nor is the plugin: the
    // game's threads may still be inside the HLE while it closes, and they keep the hub, the engine and the HLE they were using).
    if (dlp_hub_) {
        dlp_hub_->Stop();
    }

    // The network thread first, with a deadline: it may be starved or stuck in a service call, and
    // the framework gives main() about one second before it tears everything down.
    bool net_stopped = true;
    if (room_) {
        net_stopped = room_->StopWithin(kNetStopTimeoutMs);
    }
    if (net_stopped) {
        ShutdownSockets();
    } else {
        Log("shutdown: the network thread did not stop in time, leaving its memory alone");
    }
    const u32 ndm_anomalies = hle_ ? hle_->ndm_header_anomalies() : 0;
    if (ndm_anomalies != 0) {
        Log("shutdown: %lu ndm:u request(s) had a header the firmware's ndm refuses (served as usual)",
            static_cast<unsigned long>(ndm_anomalies));
    }
    if (DebugRingReady()) {
        // What the capture did over the whole launch (the "stats: capture" lines are per window of 5 s): the figure to compare
        // two builds, or two games, by.
        const CaptureTotals t = DebugCaptureTotals();
        Log("shutdown: the capture took %lu records (control %lu, heavy %lu), lost %lu heavy (ring full %lu, lock busy %lu) and"
            " %lu control (%lu, %lu), wrote %lu LOST markers; the diet counted %lu IPC calls in %lu lines; ring peak %lu KB",
            static_cast<unsigned long>(t.records[0] + t.records[1]), static_cast<unsigned long>(t.records[0]),
            static_cast<unsigned long>(t.records[1]), static_cast<unsigned long>(t.lost_full[1] + t.lost_busy[1]),
            static_cast<unsigned long>(t.lost_full[1]), static_cast<unsigned long>(t.lost_busy[1]),
            static_cast<unsigned long>(t.lost_full[0] + t.lost_busy[0]), static_cast<unsigned long>(t.lost_full[0]),
            static_cast<unsigned long>(t.lost_busy[0]), static_cast<unsigned long>(t.markers),
            static_cast<unsigned long>(t.agg_calls), static_cast<unsigned long>(t.agg_lines),
            static_cast<unsigned long>(t.ring_peak / 1024));
    }
    Log("shutdown: done");

    log_stop_.store(true);
    WakeLogWriter();
    crumb_stop_.store(true);
    if (crumb_wake_) {
        svcSignalEvent(crumb_wake_);
    }
    // One deadline for both threads (they were told together and stop side by side): the framework leaves main() about a
    // second, and the writer is the one whose end matters, so it is waited for first.
    const u64 stop_began_ms = platform::NowMs();
    const bool writer_stopped = log_thread_.JoinTimeout(kLogStopTimeoutMs);
    const u64 spent_ms = platform::NowMs() - stop_began_ms;
    crumb_thread_.JoinTimeout(spent_ms < kLogStopTimeoutMs ? static_cast<u32>(kLogStopTimeoutMs - spent_ms) : 0);
    if (writer_stopped) {
        // (A breadcrumb thread that is stuck in a write holds only the trail's lock, which Close() does not wait for.) With the
        // log off there is nothing to write and no file is open.
        log_files_->Close();
    } else {
        // The writer is still in a write to the card and holds the file lock; it stops after that write (BeginShutdown set its
        // deadline). The main log's last lines - 'shutdown: done' above all - get a short wait for the lock, not Close()'s full
        // drain: a long wait would keep the game from leaving.
        log_files_->FlushWithin(kLogStopTimeoutMs);
    }
}

// ---------------------------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------------------------

bool Plugin::LoadConfig() {
    // The old place (an app older than this build) is read while the new file is missing, never written (SaveConfig writes the new
    // place; the app moves the file).
    config_from_old_place_ = File::Exists(kConfigPath) != 1 && File::Exists(kOldConfigPath) == 1;
    File file;
    if (File::Open(file, config_from_old_place_ ? kOldConfigPath : kConfigPath, File::READ) != File::SUCCESS) {
        config_from_old_place_ = false;
        return false;
    }
    const u64 size = file.GetSize();
    if (size == 0 || size > 4096) {
        return false;
    }
    std::string content(static_cast<std::size_t>(size), '\0');
    if (file.Read(&content[0], static_cast<u32>(size)) < 0) {
        return false;
    }

    std::size_t pos = 0;
    while (pos < content.size()) {
        std::size_t end = content.find('\n', pos);
        if (end == std::string::npos) {
            end = content.size();
        }
        const ini::KeyValue kv = ini::SplitLine(content.substr(pos, end - pos));
        pos = end + 1;
        if (!kv.ok) {
            continue; // (blank, a comment, a line without '=')
        }
        const std::string& key = kv.key;
        const std::string& value = kv.value;
        if (key == "enabled") {
            config_.enabled = ini::ParseBool(value, true);
        } else if (key == "host") {
            config_.host = value;
        } else if (key == "port") {
            const long port = std::strtol(value.c_str(), nullptr, 10);
            if (port > 0 && port < 65536) {
                config_.port = static_cast<u16>(port);
            }
        } else if (key == "nickname") {
            // Older versions saved the placeholder name; treat it as "not chosen".
            config_.nickname = value == kDefaultNickname ? "" : value;
        } else if (key == "password") {
            config_.password = value;
        } else if (key == "tunnel") {
            config_.tunnel_enabled = ini::ParseBool(value, true);
        } else if (key == "autoconnect") {
            config_.autoconnect = ini::ParseBool(value, true);
        } else if (key == "radio_like") {
            config_.radio_like = ini::ParseRadioLike(value);
        } else if (key == "socket_buffers") {
            config_.socket_buffers = ini::ParseBool(value, true);
        } else if (key == "net_thread_core") {
            const long core = std::strtol(value.c_str(), nullptr, 10);
            if (core >= -1 && core <= 3) { // (2 and 3: the extra cores of a New 3DS; refused by the kernel on an Old 3DS, then -1 is tried)
                config_.net_core = static_cast<int>(core);
            }
        } else if (key == "menu_key") {
            if (!value.empty()) {
                config_.menu_key = value; // (a chord, or "off"/"none": see menu_off())
            }
        } else if (key == "game_name") {
            config_.game_name = value;
        } else if (key == "debug") {
            config_.debug = ini::DebugLevel(value); // (a 2 of 0.1.14 and older is a 1: the third level is gone)
        } else if (key == "rcvbuf_kb") {
            config_.rcvbuf_kb = ini::RcvbufKb(value); // (checked where it is used)
        } else if (key == "sleep_stop") {
            config_.sleep_stop = ini::SleepStop(value); // (0, 1, 2: see azl/ini_values.hpp)
        }
    }
    return true;
}

bool Plugin::SaveConfig() {
    // All 15 keys, always, in the order the Setup app writes them (ini::Render is the one writer of both): a file made by the
    // plugin can be edited by hand without knowing which keys exist. The advanced ones, rcvbuf_kb and sleep_stop, used to be
    // written only when they differed from the default, so a file never showed them.
    ini::Fields fields;
    fields.enabled = config_.enabled;
    fields.host = config_.host;
    fields.port = config_.port;
    fields.nickname = config_.nickname;
    fields.password = config_.password;
    fields.tunnel = config_.tunnel_enabled;
    fields.autoconnect = config_.autoconnect;
    fields.radio_like = config_.radio_like;
    fields.socket_buffers = config_.socket_buffers;
    fields.net_thread_core = config_.net_core;
    fields.menu_key = config_.menu_key;
    fields.game_name = config_.game_name;
    fields.debug = config_.debug;
    fields.rcvbuf_kb = config_.rcvbuf_kb;
    fields.sleep_stop = config_.sleep_stop;
    const std::string text = ini::Render(fields);

    File file;
    MakeDataDir();
    if (File::Open(file, kConfigPath, File::RWC | File::TRUNCATE | File::SYNC) != File::SUCCESS) {
        Log("config: cannot write %s", kConfigPath);
        return false;
    }
    return file.Write(text.data(), static_cast<u32>(text.size())) == File::SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------------------------

std::string Plugin::EffectiveNickname() const {
    std::string name = config_.nickname.empty() ? console_name_ : config_.nickname;
    if (name.empty()) {
        name = kDefaultNickname;
    }
    if (rejoin_.nickname_suffixed()) {
        // Another console with the same profile name is in the room: tell them apart by MAC.
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), "-%02X%02X", console_mac_[4], console_mac_[5]);
        if (name.size() + 5 > 20) {
            name.resize(15);
        }
        name += suffix;
    }
    return name;
}

std::string Plugin::JoinedNickname() const {
    platform::LockGuard lock(name_mutex_);
    return joined_nickname_;
}

std::string Plugin::GameName() const {
    platform::LockGuard lock(name_mutex_);
    return game_name_;
}

bool Plugin::Connect() {
    if (config_.host.empty()) {
        PostNotification("Set the room address first");
        return false;
    }
    if (!InitSockets()) {
        if (rejoin_.failures() == 0) {
            PostNotification(SocketInitFailure()); // (the reason and its code, in the words of the screen)
        }
        return false;
    }
    rejoin_.NetworkReady();
    const bool real_mac = rejoin_.prefer_real_mac();
    RoomClient::Config rc;
    rc.host = config_.host;
    rc.port = config_.port;
    rc.nickname = EffectiveNickname();
    {
        platform::LockGuard lock(name_mutex_);
        joined_nickname_ = rc.nickname;
    }
    rc.password = config_.password;
    rc.preferred_mac = real_mac ? console_mac_ : kNoPreferredMac;
    char id[32];
    std::snprintf(id, sizeof(id), "3ds-%02x%02x%02x%02x%02x%02x", console_mac_[0], console_mac_[1],
                  console_mac_[2], console_mac_[3], console_mac_[4], console_mac_[5]);
    rc.console_id_hash = id;
    rc.game_name = GameName();
    rc.game_id = game_id_;
    Log("room: connecting to %s:%u as '%s' (%s MAC, game '%s')", rc.host.c_str(), rc.port,
        rc.nickname.c_str(), real_mac ? "console's own" : "room-assigned",
        rc.game_name.c_str());
    return room_->Start(rc);
}

void Plugin::Disconnect() {
    room_->Stop();
}

bool Plugin::Tick(bool want_connected) {
    if (shutting_down_.load()) {
        return true;
    }
    const u64 now = platform::NowMs();
    SampleLinkState(now); // (the room thread is not always there when the link goes)
    if (!log_thread_.running()) {
        ServiceLog(); // no writer thread (could not be started): fall back to the caller's thread
    }
    if (want_connected != want_connected_) {
        // (the menu's switch, the autoconnect at the start, or the plugin itself switching it off after a fatal error)
        Log("menu: 'Connect to room' is now %s", want_connected ? "ON" : "OFF");
    }
    want_connected_ = want_connected;
    if (config_.host.empty() && menu_off() && (last_hint_ms_ == 0 || now - last_hint_ms_ >= kHintIntervalMs)) {
        // With the in-game menu off the address can only come from the settings: say so, now and once a minute,
        // instead of sitting idle (a message at launch is easily missed behind the game's loading screens).
        last_hint_ms_ = now;
        PostNotification("AzaharLink: no room address. Set it in the Setup app (the in-game menu is off)");
    }
    const std::string kept = announcer_.Poll(now); // (a message that came too soon after the last one)
    if (!kept.empty()) {
        PostNotification(kept);
    }
    HandleWake(now);
    if (sleep_.sleeping()) {
        return true; // the console is going to sleep, or just woke up: no new link before the wake-up is seen
    }
    if (guard_.Due(now)) {
        // The room did not take this member back in time: the pause of the network thread was not all there was to it.
        DropSession(RoomClient::Error::LostConnection);
    }

    RejoinPolicy::Input in;
    in.want_connected = want_connected;
    in.state = room_->state();
    in.error = room_->error();
    in.now_ms = now;
    in.launch_ms = start_ms_;
    in.nickname_automatic = config_.nickname.empty();
    // The console's WiFi (the shared page, a free read): the policy looks at it every 500 ms and makes no attempt to join
    // while it is down.
    in.wifi_state = *reinterpret_cast<volatile const u8*>(kSharedPageWifiState);
    const RejoinPolicy::Step step = rejoin_.Update(in);
    if (step.wifi == RejoinPolicy::WifiNote::Down) {
        Log("room: the console's WiFi is down (network state %lu): no attempt to join the room until it is back, the state"
            " is read every %lu ms",
            static_cast<unsigned long>(step.wifi_state), static_cast<unsigned long>(RejoinPolicy::kWifiPollMs));
        Announce("WiFi is down: joining the room again when it is back");
    } else if (step.wifi == RejoinPolicy::WifiNote::Back) {
        Log("room: the console's WiFi is back (network state %lu) after %lu ms: joining the room at once",
            static_cast<unsigned long>(in.wifi_state), static_cast<unsigned long>(step.wifi_down_ms));
    }
    if (step.disconnect) {
        Disconnect();
    }
    if (step.clear_error) {
        // Switching "Connect to room" off is the user's way to start over. The last error stays visible in the
        // status line meanwhile; it is forgotten when the user switches it on again (a fatal one such as a wrong
        // password would otherwise block every later attempt).
        room_->ClearError();
        last_notified_error_ = -1; // announce the next failure again
    }
    if (!want_connected || !step.alive) {
        if (guard_.Settle()) {
            DropSession(RoomClient::Error::LostConnection); // (no rejoin is coming: the session cannot be resurrected)
        }
        return step.alive;
    }
    switch (step.note) {
    case RejoinPolicy::Note::MacTaken:
        Log("room: the console's MAC is taken, retrying with a room-assigned one");
        break;
    case RejoinPolicy::Note::NameTaken:
        Log("room: name '%s' is taken, retrying with the MAC suffix", EffectiveNickname().c_str());
        break;
    case RejoinPolicy::Note::OldLink:
        Log("room: the room still lists this console's old link: waiting for it to go (up to %lu s from the loss)",
            static_cast<unsigned long>(RejoinPolicy::kOldLinkMs / 1000));
        Announce("Waiting for the room to drop the old link");
        break;
    case RejoinPolicy::Note::None:
        break;
    }
    if (step.connect && !Connect()) {
        // Not having an address, or a network layer that will not start after a few tries, is not going to fix
        // itself: stop instead of retrying (and nagging) forever.
        const bool has_host = !config_.host.empty();
        if (!rejoin_.AttemptFailed(has_host) && has_host) {
            PostNotification("Network unavailable: giving up after several failed tries");
        }
    }
    return !rejoin_.gave_up();
}

// ---------------------------------------------------------------------------------------------
// Status / notifications
// ---------------------------------------------------------------------------------------------

std::string Plugin::StatusLine() {
    const RoomClient::State state = room_->state();
    switch (state) {
    case RoomClient::State::Idle: {
        const RoomClient::Error error = room_->error();
        const u64 now = platform::NowMs();
        if (want_connected_ && !rejoin_.gave_up() && rejoin_.waiting_for_wifi()) {
            return "Waiting for the console's WiFi" +
                   (error != RoomClient::Error::None ? std::string(" - ") + RoomClient::ErrorName(error) : std::string());
        }
        // An attempt has failed or the link was lost, and the plugin is going to try again.
        if (want_connected_ && !rejoin_.gave_up() && rejoin_.attempts() > 0 &&
            (!RejoinPolicy::IsFatal(error) || rejoin_.InOldLinkWindow(now))) {
            return "Reconnecting (attempt " + std::to_string(rejoin_.attempts()) + ", next in " +
                   std::to_string((rejoin_.NextAttemptInMs(now) + 999) / 1000) + " s)" +
                   (error != RoomClient::Error::None ? std::string(" - ") + RoomClient::ErrorName(error) : std::string());
        }
        if (error != RoomClient::Error::None) {
            return std::string("Not connected - ") + RoomClient::ErrorName(error);
        }
        return "Not connected";
    }
    case RoomClient::State::Connecting:
        return "Connecting...";
    case RoomClient::State::Joining:
        return "Joining the room...";
    case RoomClient::State::Joined: {
        const room::RoomInfo info = room_->room_info();
        return "In room \"" + info.name + "\" (" + std::to_string(info.members.size()) + " players)";
    }
    }
    return "?";
}

std::string Plugin::DetailText() {
    // The message box shows 13 lines of about 45 characters and scrolls: what the page is for comes first.
    std::string text = StatusLine() + "\n";
    if (!startup_issue_.empty()) {
        bool first = true;
        for (const std::string& line : WrapLines(startup_issue_, kDetailChars)) {
            text += (first ? "!! " : "   ") + line + "\n";
            first = false;
        }
    }
    // The numbers of the last statistics window: counted at every debug level (the log has them in more detail).
    StatusNumbers numbers;
    gamemem::Reading memory;
    {
        platform::LockGuard lock(status_mutex_);
        numbers = status_numbers_;
        memory = mem_reading_;
    }
    // What the game has left of its memory, read again every 30 s (the first reading is 20 s after the start).
    text += gamemem::StatusText(memory) + "\n";
    text += FormatStatusNumbers(numbers) + "\n\n";
    if (room_->joined()) {
        const room::RoomInfo info = room_->room_info();
        text += "Players:\n";
        for (const auto& member : info.members) {
            text += " - " + member.nickname;
            if (!member.game_name.empty()) {
                text += " [" + member.game_name + "]";
            }
            text += "\n";
        }
        text += "My MAC in the room: " + MacToString(room_->mac()) + "\n";
    }
    text += "Me: " + EffectiveNickname() + " [" + GameName() + "]\n";
    text += "Hook sites: " + std::to_string(hook_stats_.send_sync_sites) + " / " +
            std::to_string(hook_stats_.close_sites);
    text += "\nUDS sessions: " + std::to_string(hle_->sessions()) +
            ", requests: " + std::to_string(hle_->requests());
    text += "\nUDS status: " + std::to_string(static_cast<unsigned>(engine_->status()));
    text += std::string("\nVersion ") + kVersion;
    return text;
}

void Plugin::PostNotification(const std::string& message) {
    if (shutting_down_.load()) {
        return; // no overlays while the game is closing (the framework draws them in the game's
                // own graphics code, which is being torn down)
    }
    platform::LockGuard lock(notify_mutex_);
    if (notifications_.size() < 16) {
        notifications_.push_back(message);
    }
}

std::vector<std::string> Plugin::TakeNotifications() {
    platform::LockGuard lock(notify_mutex_);
    std::vector<std::string> out;
    if (!shutting_down_.load()) {
        // One overlay line is about 60 characters of the framework's font: a longer message goes on several lines
        // instead of being cut off at the edge of the screen.
        for (const std::string& message : notifications_) {
            for (const std::string& line : WrapLines(message, kToastChars)) {
                out.push_back(line);
            }
        }
    }
    notifications_.clear();
    return out;
}

} // namespace ctr
} // namespace azl
