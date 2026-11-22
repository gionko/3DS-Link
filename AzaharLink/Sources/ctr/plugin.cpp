#include "ctr/plugin.hpp"

#include <CTRPluginFramework.hpp>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>

#include "azl/chat_core.hpp"
#include "azl/dlp_title_block.hpp"
#include "azl/exit_guard.hpp"
#include "azl/flow_relief.hpp"
#include "azl/one_game_host.hpp"
#include "azl/log.hpp"
#include "azl/plg_guard.hpp"
#include "azl/plugin_compare.hpp"
#include "azl/rcvbuf.h"
#include "azl/soc_plan.hpp"
#include "azl/stats.hpp"
#include "azl/swap_gate.hpp"
#include "azl/text_wrap.hpp"
#include "azl/ticks.h"
#include "azl/title_rules.hpp"
#include "azl/wallclock.hpp"
#include "azl/whole_file.hpp"
#include "ctr/boot_diag.hpp"
#include "ctr/gate_page.hpp"
#include "ctr/input_mirror.hpp"
#include "ctr/soc.hpp"
#include "ctr/swap_gate.hpp"
#include "ctr/teardown.hpp"
#include "ctr/text_search_guard.hpp"
#include "ctr/thread_core.hpp"
#include "ctr/zombie_probe.hpp"

// (libctru's weak hid function; Sources/main.cpp overrides it. No header of libctru declares it.)
extern "C" bool hidShouldUseIrrst(void);

extern "C" int azl_enet_socket_buffers; // Sources/enet/enet_3ds.c
extern "C" int azl_enet_rcvbuf_bytes;

namespace azl {
namespace ctr {

namespace {

// The framework's connection to Luma3DS's plugin loader (azl/plg_guard.hpp): made by __wrap_plgLdrInit (at the end of this file) on the keep-alive
// thread before PatchProcess, read by __wrap_PLGLDR__FetchEvent and by EarlyInit; and the line about it (EarlyInit: the synchronous file, then the log).
plgguard::State g_plg_guard;
char g_plg_line[320];
int g_plg_line_body = -1; ///< where the line's text begins in g_plg_line, after the clock (-1: no line)

// SWAP_ENTER: the Download Play hub's requester that waits in its queue in slices sleeps no longer once the svc hook's gate is down (CloseGate).
void WakeHubForSwap(void* hub) {
    static_cast<DlpHub*>(hub)->WakeForSwap();
}

using CTRPluginFramework::File;

// The plugin's own files - the settings and the logs - live with the app's, in /3ds/3DSLink/ (the plugin binaries stay where
// Luma3DS loads them from, /luma/plugins/<title id>/). The settings used to be in /luma/plugins/AzaharLink.ini: that file is
// still read when the new one is missing (an app older than this build wrote it there; the app moves it the first time it runs).
constexpr const char* kDataDir = "/3ds/3DSLink/";
constexpr const char* kConfigPath = "/3ds/3DSLink/AzaharLink.ini";
constexpr const char* kOldConfigPath = "/luma/plugins/AzaharLink.ini";
// F7: the old file a save takes aside (azl/whole_file.hpp, whole::BakOf(kConfigPath), the app's name): alone on the card when a save was cut
// between its two renames, and then it is the settings.
constexpr const char* kConfigBakPath = "/3ds/3DSLink/AzaharLink.ini.bak";
// The memory figure of the last launch (title id, KB used, KB free of the application region at the start): written with the
// log on only, read at the next start (CompareMemoryWithLastLaunch).
constexpr const char* kMemPath = "/3ds/3DSLink/AzaharLink.mem";
// Where the startup-occupancy warning and the line of the framework's link to Luma3DS's plugin loader are written synchronously, even at
// debug=0 (the only lines the plugin ever writes with the log off): the main log's own file, so a tester who turns the log on later finds
// them in sequence.
constexpr const char* kLogSyncPath = "/3ds/3DSLink/AzaharLink.log";
// The exit guard's line and the watch of Luma3DS's plugin word (REGISTRO, phase Q): a file of their own, written synchronously at any debug
// level when they have something to say (and every exit with debug=1), never while the log's writer appends to its own file.
constexpr const char* kLumaLogPath = "/3ds/3DSLink/AzaharLink.luma.log";
// The Download Play application - its six regional titles, kDownloadPlayTitles of ctr/text_search_guard.hpp: the list the guard of GetTextSize answers, and no
// other - is the one application that gets a line of its own in that file at its start (EarlyInit). Stage G0 of Mario Party: Island Tour
// (Sources/ctr/text_search_guard.cpp lets the framework start inside it): what the card holds if the framework aborts there again.
constexpr u32 kLumaWatchLines = 64;

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
// publisher's screen and on leaving a lobby; nothing froze with it off). Its CPU is small and its writes block it in the file
// system, which is when the game runs; the formatting of a 64 KB block of the capture is NOT small in one go (26 ms measured on
// the console, 2026-11-10, not "about a millisecond" as this comment said), so a pass gives the core up every 2 ms of it
// (LogFileLimits::capture_yield_ms).
constexpr int kLogWriterPriority = 0x2E;
constexpr u32 kNetStopTimeoutMs = 400;
// Process events of the framework (Plugin::NoteProcessEvent, CTRPluginFramework's Process::Event): the index of
// EXIT, of the two sleep ones and of the two swap ones.
constexpr int kEventExit = 0;
constexpr int kEventSleepEnter = 1;
constexpr int kEventSleepExit = 2;
constexpr int kEventHomeEnter = 3;
constexpr int kEventHomeExit = 4;
constexpr int kEventSwapEnter = 5;
constexpr int kEventSwapExit = 6;
constexpr u32 kLogStopTimeoutMs = 400;
// SWAP_ENTER (Old 3DS, HOME), with the log on: how long the trail gets to put the event on the card before the threads stop.
constexpr u32 kSwapTrailWaitMs = 40;
// The close command (12) of APT:ReceiveParameter, with the log on, a New 3DS only: how long the game's thread gives the trail to put the close and Luma3DS's
// word on the card before the game can be killed (Plugin::WaitForCloseTrail; the thread writes in milliseconds when the card is not busy).
constexpr u32 kCloseTrailWaitMs = 200;
// SWAP_ENTER (Old 3DS, HOME): how long the plugin waits for the game's threads counted in the svc hook's gate to leave the plugin's code before it lets Luma3DS free
// the plugin's block. Luma3DS itself waits 10 s for the answer (waitForReplyTimeout); the threads leave in microseconds: a real service's call is made by the gate
// outside the count, and every wait of the plugin's own on a game thread ends when the gate closes (ctr/gate_flag.hpp). The budget is for what is left.
constexpr u32 kSwapDrainTimeoutMs = 1500;
// ... and for the plugin's own threads to leave their service calls (azl/swap_gate.hpp): a poll() lasts 96 ms at most in the logs, a write of the log's writer can take a second and more on a slow card,
// and a getaddrinfo with no DNS takes seconds. Before this wait the chat's mirror may take 1 s (kEventLockTicks), the trail 40 ms (kSwapTrailWaitMs) and the hooks 1.5 s
// (kSwapDrainTimeoutMs): 8.64 s in all with this one. Every one of them also stops at SWAP_ENTER's one deadline (ctr/swap_gate.hpp), counted from Luma3DS's event:
// 8 s when Luma3DS waits its 10 s, 12.5 s once the wait is raised to 15 s (RaiseRosalinaReplyWait), so the budgets above are whole then. (A call that lasts longer: the swap
// then goes on as it did before the gate; the debug log says who was left, if the console survives it.)
constexpr u32 kSwapGateTimeoutMs = 6100;
constexpr u32 kRosalinaReplyWaitOffset = 0x28;               // PluginHeader.waitForReplyTimeout (Luma3DS plgldr.h): RaiseRosalinaReplyWait
constexpr u32 kLoaderWordsOffset = 0x80;                     // PluginHeader.config[32] (Luma3DS plgldr.h): a plg:ldr command 4's words, NotePreparedChild
constexpr u64 kRosalinaReplyWaitNs = 10000000000ull;        // what Luma3DS writes there (file_loader.c)
constexpr u64 kRosalinaReplyWaitRaisedNs = 15000000000ull;  // Old 3DS: 15 s

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
// 1.1.4 (route B, spec 8 D-B9): how long from the launch the prepared Download Play child keeps the console's MAC and name in the room through a
// collision: the 3DS Link app left it with them a moment before (its exit takes about 3 s at the worst; past 8 s the child's 10 rounds on an
// Azahar host are over anyway).
constexpr u32 kChildHoldIdMs = 8000;

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

// The Download Play column of each game is in azl/title_rules.hpp (Dlp, DlpOf: the application's id only, every region of a game in its one
// row; the header says what each mode, the seed and the ceiling do); these are the plugin's names for it.
DlpGameMode DlpModeOf(u64 title_id) {
    if (IsDownloadPlayTitle(title_id)) { return DlpGameMode::SystemClient; }  // 1.1.4 (G1): the system's Download Play application (ctr/text_search_guard.hpp's six ids): no row
    return title_rules::DlpOf(title_id).mode;
}

/// 1.1.4 (route B, spec 8 D-B6): is a room member, by its game id, a guest whose child a host of the real Download Play waits for at its final?
/// One in the system's Download Play application (ctr/text_search_guard.hpp's six ids) or in the 3DS Link app's Download Play page
/// (title_rules::kAppTid): both leave the room once the passphrase came and come back as the prepared child. Any other id: no.
bool AwaitsChildOf(u64 game_id) {
    return IsDownloadPlayTitle(game_id) || game_id == title_rules::kAppTid;
}

/// 1.1.4 (G1, contract K5): does the system's Download Play application keep the system's own Download Play (the plugin stays out of it: no hooks,
/// ndm:u and dlp:CLNT are the real ones, local Download Play and 3DS Link Delivery work)? Yes when the settings were not read at the start (no room
/// can be known: the safe side) or when the plugin would not join a room by itself (clnt::UsesRoom, Sources/main.cpp's formula). Decided once, from
/// the settings, in EarlyInit: the application's first touch comes before the room is joined (4.6 s against 5.7 s on a console), so the room's own
/// state can never decide it. Every other title: false.
bool ClntStaysLocal(u64 title_id, bool config_read_early, bool host_set, bool autoconnect, bool menu_off) {
    return DlpModeOf(title_id) == DlpGameMode::SystemClient && (!config_read_early || !clnt::UsesRoom(host_set, autoconnect, menu_off));
}
static_assert(title_rules::kDlpTitlePlayers == kTitlePlayers && title_rules::kDlpPlayersMax == kMaxPlayers,
              "the ceilings of the Download Play column are the lobby's: four unless the row says more, eight at the most");

/// Does the lobby of this game announce the console's own friend code seed (config block 0x00090000, 8 bytes: what the game itself
/// reads for nwm::UDS) instead of the hash of the MAC? Where the game compares the two (Mario Tennis Open) and where a real console's
/// Download Play would give it and nothing speaks against it (Mario Kart 7, the real Download Play); false for every other game.
bool LobbySeedIsConsoles(u64 title_id) {
    return title_rules::DlpOf(title_id).console_seed;
}

/// The ceiling of players (host included) of this game's lobby: what its row says - four unless the row says more (Mario Kart 7: eight) -
/// and four for a game that has no row (kNoDlp).
u8 LobbyPlayersOf(u64 title_id) {
    return title_rules::DlpOf(title_id).max_players;
}

/// Does the host of this game's OWN network (nwm::UDS, where the players go after the Download Play) send the Logoff of an admission to
/// the members of the network one by one instead of broadcasting it (UdsEngine::SetAdmissionToMembers)? The rows of the real Download
/// Play that say more than four players - Mario Kart 7's: its players come back from the Download Play all within a tenth of a
/// second, and an Azahar that is Connecting adopts any Logoff it hears (the capture of eight Azahar of 2026-11-13: three of seven took
/// another's node id, and the two whose id was taken fell out of the race). False for every other game - Luigi's Mansion among them:
/// one client, nobody whose Logoff it could take - and for a game that has no row: their frames are what they were.
bool AdmissionToMembersOf(u64 title_id) {
    const title_rules::Dlp dlp = title_rules::DlpOf(title_id);
    return dlp.mode == DlpGameMode::RealDlp && dlp.max_players > kTitlePlayers;
}

/// A game of the probe's kind (Battle Royale): dlp:FKCL is answered by the plugin's probe, nothing else of Download Play is.
bool AsksDlpOnlyForTheWireless(u64 title_id) {
    return DlpModeOf(title_id) == DlpGameMode::ProbeOnly;
}

/// 1.1.4 (G4, contract K8): is `own` the Download Play child 3DS Link prepared, and whose? Only a title of the band (title_rules::IsPreparedId) whose
/// row the index has - the row whose `prepared` is `own` (dlp_children::FindPrepared: who a prepared title is, is the index's, never a formula read
/// backwards) -, copied to `*row`. Any other title, and one of the band with no row (no index, a line the parser refused, 3DS Link and the homebrew
/// of the band), is not: the plugin runs as for any title it does not know. `text` is the index as Plugin::ReadDataFile gives it (nullptr: none);
/// `scratch` is the caller's (an Index is about 520 bytes). Pure.
bool PreparedChildRow(u64 own, const char* text, std::size_t size, dlp_children::Index* scratch, dlp_children::Row* row) {
    // (the build 2h) the process's id is the system id 00040010|low of the child (or the key itself, 00040000|low: the fallback form and the
    // children of the builds before): the index names the KEY, so the row is looked for by it - never 00040001, never a title out of the band
    const u64 slot = title_rules::PreparedSlotOfProcess(own);
    if (slot == 0 || text == nullptr || scratch == nullptr || !dlp_children::ParseIndex(text, size, scratch)) {
        return false;
    }
    const int r = dlp_children::FindPrepared(*scratch, slot);
    if (r < 0) {
        return false;
    }
    *row = scratch->rows[r];
    return true;
}

/// 1.1.4 (G4): does the prepared child join the room in EarlyInit (Plugin::EarlyConnect), before the game's first instruction? Its host's game waits
/// 20 s for the players it counted. Only that title, only with the plugin on and where it joins a room by itself (clnt::UsesRoom: Sources/main.cpp's
/// formula, from settings read at the start), and only on a New 3DS: an Old 3DS's gate of the plugin's threads is made in LateInit, so there the room
/// is joined at the first frame, as for every title.
bool JoinsEarly(bool prepared_child, bool active, bool new_console, bool config_read_early, bool host_set, bool autoconnect, bool menu_off) {
    return prepared_child && active && new_console && config_read_early && clnt::UsesRoom(host_set, autoconnect, menu_off);
}

/// 1.1.4 (G4, review): what the game is told by the dlp:SRVR of the prepared child, for its line of the log. The plugin's service answers only through
/// the hooks: with the plugin off for this launch, or the hooks out (no gate page, the diagnostic build's switch), the game talks to the real dlp:SRVR,
/// which says "not a child" whatever service the plugin made.
const char* ChildSrvrLine(bool has_service, bool active, bool hooked) {
    return !active   ? "the real one (the plugin is off for this launch): IsChild says false"
           : !hooked ? "the real one (no hooks for this launch): IsChild says false"
           : has_service ? "answered by the plugin: IsChild says TRUE"
                         : "the real one (a hub, or no memory for its service): IsChild says false";
}

/// 1.1.4 (G4): the 32 words Luma3DS's plugin loader put in the plugin's header for this launch (PluginHeader.config: what a plg:ldr command 4 gave,
/// the prepared child's 16 bytes of the jump - G3), taken once: copied to `shelter` and wiped from the header, which nothing else then reads. True
/// when one of them is not 0. The words never go to the log.
constexpr std::size_t kLoaderWords = 32;
bool TakeLoaderWords(volatile u32* header_words, u32 (&shelter)[kLoaderWords]) {
    bool any = false;
    for (std::size_t i = 0; i < kLoaderWords; ++i) {
        shelter[i] = header_words[i];
        header_words[i] = 0;
        any = any || shelter[i] != 0;
    }
    return any;
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
/// "3DS Link" (the game's InitializeWithName replaces it); the ceiling of players is the row's (four, eight for Mario Kart 7).
std::unique_ptr<DlpHub> MakeDlpHub(UdsEngine* engine, KernelOps* kernel, u64 game_tid, const Mac& console_mac) {
    LobbyConfig config = {};
    config.game_tid = game_tid;
    config.friend_seed = Fnv1a64(console_mac.data(), console_mac.size());
    static const char kDefaultName[] = "3DS Link";
    for (std::size_t i = 0; i < kNameUnits && kDefaultName[i] != '\0'; ++i) {
        config.name[i] = static_cast<u16>(kDefaultName[i]);
    }
    config.max_players = LobbyPlayersOf(game_tid);
    IpcResolver resolver;
    resolver.ctx = nullptr;
    resolver.map = &MapGameAddress;
    return std::unique_ptr<DlpHub>(new (std::nothrow) DlpHub(engine, kernel, config, resolver));
}

/// The hub of a game in RealDlp mode, or null (no memory: the game keeps the real services): the configuration of MakeDlpHub, word for word, and
/// the adapter of the real Download Play as the lobby's network (`real`, made by the caller and kept alive by it), told the ceiling of players of
/// the row as the lobby is (four, eight for Mario Kart 7); the engine is told how this game's own network admits (AdmissionToMembersOf:
/// to the members one by one for Mario Kart 7, broadcast as ever for the others). The probes of the searcher take
/// the engine as a spectator with the identity the lobby starts with (the hash of the MAC, "3DS Link"): nobody shows a spectator's.
std::unique_ptr<DlpHub> MakeRealDlpHub(UdsEngine* engine, KernelOps* kernel, u64 game_tid, const Mac& console_mac, RealDlpNet* real) {
    LobbyConfig config = {};
    config.game_tid = game_tid;
    config.friend_seed = Fnv1a64(console_mac.data(), console_mac.size());
    static const char kDefaultName[] = "3DS Link";
    for (std::size_t i = 0; i < kNameUnits && kDefaultName[i] != '\0'; ++i) {
        config.name[i] = static_cast<u16>(kDefaultName[i]);
    }
    config.max_players = LobbyPlayersOf(game_tid);
    IpcResolver resolver;
    resolver.ctx = nullptr;
    resolver.map = &MapGameAddress;
    Identity probe = {};
    probe.seed = config.friend_seed;
    for (std::size_t i = 0; i < kNameUnits; ++i) {
        probe.name[i] = config.name[i];
    }
    real->SetIdentity(probe);
    real->SetPlayers(config.max_players);
    real->SetChildWait(DlpModeOf(game_tid) == DlpGameMode::RealDlp);  // (1.1.4, H5: a game that hosts the real Download Play waits in its state 9 for the guests of the Download Play application that went to start their child)
    engine->SetAdmissionToMembers(AdmissionToMembersOf(game_tid));  // (the game's own network after the lobby: Mario Kart 7 only)
    return std::unique_ptr<DlpHub>(new (std::nothrow) DlpHub(engine, kernel, config, resolver, real));
}

/// The player's name of the Download Play application's lobby until the profile's is known (the probes' spectator name too).
constexpr char kClntPlayerName[] = "3DS Link";

/// 1.1.4 (G1): the hub of the system's Download Play application, or null (no memory: the application keeps the system's Download Play). The
/// searcher of the real Download Play (`real`, made by the caller) is the lobby's network and feeds `book`, the book of dlp:CLNT; the lobby lists
/// ANY game's host (LobbyConfig::any_title), up to the tables' eight players; the engine's admissions stay as they are (the application has no
/// network of its own after the lobby). The application never hosts: nothing of the creator is used.
std::unique_ptr<DlpHub> MakeClntHub(UdsEngine* engine, KernelOps* kernel, u64 app_tid, const Mac& console_mac, RealDlpNet* real, DlpClntBook* book) {
    LobbyConfig cfg = {};
    cfg.game_tid = app_tid;
    cfg.friend_seed = Fnv1a64(console_mac.data(), console_mac.size());
    for (std::size_t k = 0; k < kNameUnits && kClntPlayerName[k] != '\0'; ++k) {
        cfg.name[k] = static_cast<u16>(kClntPlayerName[k]);
    }
    cfg.max_players = static_cast<u8>(kMaxPlayers);  // (any game's lobby: the tables' ceiling, not a title's)
    cfg.any_title = 1;
    IpcResolver map;
    map.ctx = nullptr;
    map.map = &MapGameAddress;
    Identity spectator = {};
    spectator.seed = cfg.friend_seed;
    std::memcpy(spectator.name, cfg.name, sizeof(spectator.name));
    real->SetIdentity(spectator);
    real->SetPlayers(cfg.max_players);
    real->SetClntBook(book);
    return std::unique_ptr<DlpHub>(new (std::nothrow) DlpHub(engine, kernel, cfg, map, real));
}
// <<< end of the Download Play wiring (Tests/plugin_dlp_extract.py)

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

/// D-S1 (2026-11-15, the console probe of the Download Play client, analysis/piano-1.1.4-2026-11-15/mappa/dlp-client.md): in the system's Download Play
/// application ONLY, what its process may open - a game's content 2 (its Download Play child, what tells "installed") on the SD and on the card, the
/// prepared child (an application: Luma3DS loads plugins into applications only) and the service am:u. Two lines of results for each game of the index
/// of the children (the first kProbeRows), then the line of am:u; every handle closed at once; nothing is read, nothing changes. 1.1.4 (G4, K1): the
/// games are the index's and their prepared titles title_rules::PreparedOf of the GAME - the one formula; the first version derived the game from a
/// fixed prepared id with a mask that lost the high bits of the Taiwanese and Korean low words.
constexpr std::size_t kProbeRows = 4;
__attribute__((noinline)) void ProbeDownloadPlayOpens() {
    auto open_content = [](u64 tid, FS_MediaType media, u32 content) -> Result {
        u32 archive_path[4] = {static_cast<u32>(tid), static_cast<u32>(tid >> 32), static_cast<u32>(media), 0};
        u32 file_path[5] = {0 /* NCCH data */, content, 0 /* its RomFS */, 0, 0};
        const FS_Path archive = {PATH_BINARY, sizeof(archive_path), archive_path};
        const FS_Path file = {PATH_BINARY, sizeof(file_path), file_path};
        Handle handle = 0;
        const Result rc = FSUSER_OpenFileDirectly(&handle, ARCHIVE_SAVEDATA_AND_CONTENT, archive, file, FS_OPEN_READ, 0);
        if (R_SUCCEEDED(rc)) {
            FSFILE_Close(handle);
        }
        return rc;
    };
    std::string text;
    std::unique_ptr<dlp_children::Index> index(new (std::nothrow) dlp_children::Index);
    const bool listed = index && Plugin::ReadDataFile(dlp_children::kIndexPath, dlp_children::kIndexMaxBytes, text) &&
                        dlp_children::ParseIndex(text.data(), text.size(), index.get());
    const std::size_t games = listed ? index->count : 0;
    if (games == 0) {
        Log("dlp probe: no game in the index of the children (%s): no content to open", dlp_children::kIndexPath);
    }
    for (std::size_t r = 0; r < games && r < kProbeRows; ++r) {
        const u64 game = index->rows[r].game;
        const u64 prepared_tid = title_rules::PreparedOf(game);  // (K1: the one formula, on the game)
        const Result game_sd = open_content(game, MEDIATYPE_SD, 2 /* its Download Play child */);
        const Result game_card = open_content(game, MEDIATYPE_GAME_CARD, 2);
        Log("dlp probe: from the Download Play application, the content 2 of %08lx%08lx: SD %s, card %s",
            static_cast<unsigned long>(game >> 32), static_cast<unsigned long>(game), DescribeResult(game_sd).c_str(), DescribeResult(game_card).c_str());
        const Result prepared = open_content(prepared_tid, MEDIATYPE_SD, 0);
        Log("dlp probe: the prepared child %08lx%08lx, content 0 on the SD: %s", static_cast<unsigned long>(prepared_tid >> 32),
            static_cast<unsigned long>(prepared_tid), DescribeResult(prepared).c_str());
    }
    Handle am = 0;
    const Result am_rc = srvGetServiceHandle(&am, "am:u");
    if (R_SUCCEEDED(am_rc)) {
        svcCloseHandle(am);
    }
    Log("dlp probe: the service am:u from the Download Play application: %s", DescribeResult(am_rc).c_str());
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

// The title in the announcement of the real Download Play (2026-11-14, azl/dlp_title_block.hpp): the names, the icon and the sizes of
// the child that a real host announces, for every game whose host has the real Download Play (2026-11-15: no list, as the system's own service
// does; dlp_real::AnnouncesTitle). Read from the title that is RUNNING - no table of names - once, at start, on
// the plugin's own thread (LateInit: no thread of the game ever waits for a file here, and the room's thread, the only one that
// builds the announcement, does not exist yet), into one block of 5 KB that is kept for as long as the adapter is (the user's choice: 5 KB of
// the plugin's own memory, not the game's) and that a game with no hub never has. Whatever fails, the announcement is zeros and the game is told nothing.

/// The content of a title that holds its Download Play children (the "child container": a RomFS with one CIA per child).
constexpr u32 kDlpChildContent = 2;

/// FSFILE_Read as dlp_real::ReadAt wants it (`ctx` = the file's handle): every byte asked for, or false.
bool ReadFileAt(void* ctx, u64 offset, u8* out, u32 size) {
    u32 read = 0;
    return R_SUCCEEDED(FSFILE_Read(*static_cast<Handle*>(ctx), &read, offset, out, size)) && read == size;
}

// (Out of line: LateInit, which every game runs, keeps the frame it had.)
__attribute__((noinline)) void GiveAnnouncementItsTitle(RealDlpNet* real, u64 game_id, u8 language) {
    const u64 began_ms = platform::NowMs();
    dlp_real::TitleBlock* block = new (std::nothrow) dlp_real::TitleBlock;
    u8* smdh = new (std::nothrow) u8[dlp_real::kSmdhBytes];  // (14 KB, given back below)
    if (block == nullptr || smdh == nullptr || R_FAILED(fsInit())) {
        Log("dlp: the announcement stays without the game's name, icon and sizes (zeros, as before): %s",
            block == nullptr || smdh == nullptr ? "no memory for them" : "fsInit failed");
        delete block;
        delete[] smdh;
        return;
    }
    std::memset(block, 0, sizeof(*block));
    block->child_tid_low = static_cast<u32>(game_id) & 0xFFFFFF00u;  // (the child 0: the announcement of another child gets none of this)
    // 1. As a real host does: the child's package in the game's content 2, on the media the game was started from, then the other one.
    //    Its sizes, and its own names and icon. (This archive has been read from a game's process on consoles - the icon of another
    //    title, content 0; the RomFS of a content 2 never: the line below says how it went, on each media.)
    FS_MediaType first = MEDIATYPE_SD;
    const Result media_rc = FSUSER_GetMediaType(&first);
    const FS_MediaType from = R_SUCCEEDED(media_rc) && (first == MEDIATYPE_SD || first == MEDIATYPE_GAME_CARD) ? first : MEDIATYPE_SD;
    const FS_MediaType medias[2] = {from, from == MEDIATYPE_SD ? MEDIATYPE_GAME_CARD : MEDIATYPE_SD};
    const char* names_from = nullptr;
    dlp_real::PackageWhy why = dlp_real::PackageWhy::Read;
    Result open_rc[2] = {0, 0};  // (one for each media: the refusal of the first one is not lost under the second one's)
    int tried = 0;
    unsigned long names_entry = dlp_real::kSmdhTitles;  // the SMDH's entry the names are of: the console's language, or the one in its place
    dlp_real::ChildPackage package = {};
    bool opened = false;       // (the first media where the content opens is the one: its verdict is the line's)
    int sized_on = -1;
    u64 opened_size = 0;       // of a package that opened and was not read: its length ...
    u8 opened_head[8] = {};    // ... and its first two words (what the file system gives for a content's RomFS)
    for (int i = 0; i < 2 && !opened; ++i) {
        u32 archive_path[4] = {static_cast<u32>(game_id), static_cast<u32>(game_id >> 32), static_cast<u32>(medias[i]), 0};
        u32 file_path[5] = {0 /* NCCH data */, kDlpChildContent, 0 /* its RomFS */, 0, 0};
        const FS_Path archive = {PATH_BINARY, sizeof(archive_path), archive_path};
        const FS_Path file = {PATH_BINARY, sizeof(file_path), file_path};
        Handle handle = 0;
        tried = i + 1;
        open_rc[i] = FSUSER_OpenFileDirectly(&handle, ARCHIVE_SAVEDATA_AND_CONTENT, archive, file, FS_OPEN_READ, 0);
        if (R_FAILED(open_rc[i])) {
            continue;
        }
        opened = true;
        why = dlp_real::FindChildPackage(&ReadFileAt, &handle, block->child_tid_low, &package);
        if (why == dlp_real::PackageWhy::Ok) {
            block->transfer_size = package.transfer_size;
            block->required_size = package.required_size;
            sized_on = static_cast<int>(medias[i]);
            if (ReadFileAt(&handle, package.smdh_at, smdh, dlp_real::kSmdhBytes) &&
                dlp_real::TitleFromSmdh(block, smdh, dlp_real::kSmdhBytes, language)) {
                names_from = "the child's package";
                names_entry = dlp_real::SmdhTitleEntry(smdh, language);
            }
        } else {
            FSFILE_GetSize(handle, &opened_size);
            ReadFileAt(&handle, 0, opened_head, sizeof(opened_head));
        }
        FSFILE_Close(handle);
    }
    // 2. Where that gave no names: the game's own icon file (the read ReadIconSelfNcch makes in every game), whole.
    Result icon_rc = 0;
    if (names_from == nullptr) {
        const FS_Path self = {PATH_EMPTY, 1, ""};
        u32 icon_words[3] = {2 /* ExeFS / system menu data */, 0x6E6F6369 /* "icon" */, 0};
        const FS_Path icon = {PATH_BINARY, sizeof(icon_words), icon_words};
        Handle handle = 0;
        icon_rc = FSUSER_OpenFileDirectly(&handle, ARCHIVE_ROMFS, self, icon, FS_OPEN_READ, 0);
        if (R_SUCCEEDED(icon_rc)) {
            u32 read = 0;
            icon_rc = FSFILE_Read(handle, &read, 0, smdh, dlp_real::kSmdhBytes);
            FSFILE_Close(handle);
            if (R_SUCCEEDED(icon_rc) && dlp_real::TitleFromSmdh(block, smdh, read, language)) {
                names_from = "the game's own icon";
                names_entry = dlp_real::SmdhTitleEntry(smdh, language);
            }
        }
    }
    fsExit();
    delete[] smdh;
    const unsigned long took_ms = static_cast<unsigned long>(platform::NowMs() - began_ms);
    if (sized_on < 0) {
        // (a line of its own, straight to the log: what each media answered, what the file that opened looks like, what the first file is called)
        char second[12] = "not tried";
        if (tried > 1) {
            std::snprintf(second, sizeof(second), "0x%08lx", static_cast<unsigned long>(open_rc[1]));
        }
        Log("dlp: the child's package was not read: %s; open 0x%08lx on media %d, %s on media %d; what opened is 0x%08lx bytes and starts"
            " %08lx %08lx; its first file is '%s', a name of %lu bytes",
            !opened ? "it did not open" : dlp_real::PackageWhyText(why), static_cast<unsigned long>(open_rc[0]),
            static_cast<int>(medias[0]), second, static_cast<int>(medias[1]), static_cast<unsigned long>(opened_size),
            static_cast<unsigned long>(LoadBE32(opened_head)), static_cast<unsigned long>(LoadBE32(opened_head + 4)),
            package.first_name, static_cast<unsigned long>(package.first_name_bytes));
    }
    if (names_from == nullptr && sized_on < 0) {
        delete block;
        Log("dlp: the announcement stays without the game's name and icon (zeros, as before): the game's own icon was not read (0x%08lx)"
            " and neither was the child's package (the line before); %lu ms", static_cast<unsigned long>(icon_rc), took_ms);
        return;
    }
    char sized[112];
    const char* sizes = "NO sizes (zeros: the child's package was not read, the line before says why)";
    if (sized_on >= 0) {
        std::snprintf(sized, sizeof(sized), "the child's sizes 0x%08lx and 0x%08lx from its package (content %lu, media %d)",
                      static_cast<unsigned long>(block->transfer_size), static_cast<unsigned long>(block->required_size),
                      static_cast<unsigned long>(kDlpChildContent), sized_on);
        sizes = sized;
    }
    real->SetTitleBlock(block);  // (from here the adapter's: never freed, never changed - as the adapter and the hub themselves)
    Log("dlp: the announcement carries the game's name and icon from %s (language %lu, the names of entry %lu) and %s; %lu ms, %lu bytes kept",
        names_from != nullptr ? names_from : "NOWHERE (zeros: neither the package's nor the game's own icon was read)",
        static_cast<unsigned long>(language), names_entry, sizes, took_ms, static_cast<unsigned long>(sizeof(*block)));
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

/// The card for azl/whole_file.hpp (F7): AzaharLink.ini goes down whole or not at all, in the order of the app's WriteTextFile. CTRPF's
/// File::Rename is FSUSER_RenameFile alone (it never replaces: the order never asks it to); Rename and Remove answer File::SUCCESS (0) or the
/// FS error (File.hpp of CTRPF 0.8.0), Exists 1 for a file that is there. F7-rev (File.o of 0.8.0, disassembled): Exists is one open of the
/// card in read: 1 when it opens, 0 whenever it fails - a missing file and a passing error of the card alike - and -1 only for a path it
/// cannot make; so `== 1`, and whole::Choose looks at the ini once more before it takes the .bak.
struct CtrWhole {
    bool Exists(const std::string& path) { return File::Exists(path) == 1; }
    bool Rename(const std::string& from, const std::string& to) { return File::Rename(from, to) == File::SUCCESS; }
    void Remove(const std::string& path) { File::Remove(path); }
    bool WriteNew(const std::string& path, const std::string& text) {
        {
            File file;
            if (File::Open(file, path, File::RWC | File::TRUNCATE | File::SYNC) != File::SUCCESS) {
                return false;
            }
            if (file.Write(text.data(), static_cast<u32>(text.size())) != File::SUCCESS) {
                return false;
            }
        } // (closed here: FSFILE_Close)
        std::string back;
        return Plugin::ReadDataFile(path.c_str(), text.size() + 1, back) && back == text;
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

void Plugin::SetChatOn(bool on) {
    config_.chat = on;
    chat_on_.store(on, std::memory_order_relaxed);
    WireChat(on);
    SaveConfig();
    Log("chat: %s (from the in-game menu)", on ? "ON" : "OFF");
}

void Plugin::WireChat(bool on) {
    if (!active_ || !room_) {
        return; // (the plugin off: nothing of the chat exists, whatever the switch says)
    }
    ChatSink* sink = chat_sink_.load(std::memory_order_relaxed);
    if (on && !sink) {
        // ONE block for the whole life of the plugin, made the first time the chat is on and never deleted - like Plugin::Get:
        // a network thread that RoomClient::Stop gave up on may still hold it, and the drawing reads it from the game's thread.
        // From the plugin's heap, never the game's memory.
        sink = new (std::nothrow) ChatSink();
        if (!sink) {
            Log("chat: no memory for its %lu bytes: the chat stays off for this launch", static_cast<unsigned long>(sizeof(ChatSink)));
            return;
        }
        chat_sink_.store(sink, std::memory_order_release);
        Log("chat: %lu bytes for the lines and the box, kept until the game ends", static_cast<unsigned long>(sizeof(ChatSink)));
    }
    room_->SetChatSink(on ? sink : nullptr);
}

void Plugin::SetChatPos(int pos) {
    pos = ini::ValidChatPos(pos);
    config_.chat_pos = pos;
    chat_pos_.store(pos, std::memory_order_relaxed);
    SaveConfig();
    Log("chat: position %s (from the in-game menu)", ini::ChatPosWord(pos));
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
                bool capture_early = false; // something asked for the capture to be written at once (the HOME button)
                bool master_checked = false; // the plugin of 3DS Link's folder was compared with this game's copy (once, a while after the start)
                while (!log_stop_.load()) {
                    // Once a second is plenty for the statistics window (all there is to do with the log
                    // off). With the log on the writer wakes every 250 ms for the main log, and sooner when the
                    // capture has something for the card and waits for the card's rest to end, or stepped aside
                    // for a file read of the game (LogFiles::WriterWaitMs: 20 ms at least). A stop wakes it at once.
                    const u32 wait_ms = g_debug_level > 0 ? log_files_->WriterWaitMs(platform::NowMs()) : 1000;
                    const u64 asleep = platform::NowTicks();
                    if (log_wake_) {
                        svcWaitSynchronization(log_wake_, static_cast<s64>(wait_ms) * 1000000LL);
                    } else {
                        platform::SleepMs(250);
                    }
                    // A pass is many calls to the card with their buffers in the plugin's memory: none may be under way when an Old 3DS swaps that memory out (azl/swap_gate.hpp).
                    const SwapGateScope gate(kGateWriter);
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
                    const u64 yielded_before = log_files_->yielded_ticks();
                    ServiceLog();
                    if (g_debug_level > 0) {
                        LogProcessEvents();
                        if (log_flush_requested_.exchange(false)) {
                            log_files_->Flush(); // (the HOME button: see OnHomeButton)
                            capture_early = true;
                        }
                        // The capture goes to the card in few passes (LogFileLimits::capture_every_ms), ONE write each, and leaves the
                        // card to the game (2026-11-10: the file system serves one call at a time, and with a capture the card could not
                        // keep up with the writer held the card 97% of the time - Kirby Battle Royale read one block of its files per
                        // pass and the session fell). LogFiles decides: a pass when one is due, not before the card has rested as long
                        // as the last pass kept it (20 ms..5 s), and not while the game reads its files (10 s at the most, then one
                        // small slice); the HOME button passes at once. The ring is left to fill meanwhile (it is the plugin's memory).
                        const LogFiles::CaptureTurn turn = log_files_->NextCaptureTurn(platform::NowMs(), DebugRingUsedBytes(), capture_early);
                        if (turn != LogFiles::CaptureTurn::kWait) {
                            capture_early = false;
                            log_files_->DrainDebug(false, turn == LogFiles::CaptureTurn::kSmallPass);
                        }
                    }
                    // The pass is CPU except for the time its writes spent in the SD card (a wait on the file system's
                    // process, 64% of this thread's wall time in PROVA 2) and for the milliseconds a pass of the capture gave
                    // the core up while it formatted (2026-11-10: a sleep, neither CPU nor card - charged as CPU it would
                    // show the cure as the illness in the "writer" figure of the statistics).
                    const u64 wall_ticks = platform::NowTicks() - woke;
                    const u64 yielded_ticks = log_files_->yielded_ticks() - yielded_before;
                    const u64 pass_ticks = wall_ticks > yielded_ticks ? wall_ticks - yielded_ticks : 0;
                    const u64 card_ticks = log_files_->card_wait_ticks() - card_before;
                    const u64 waited_ticks = card_ticks < pass_ticks ? card_ticks : pass_ticks;
                    StatsAdd64(g_stats.t_writer, pass_ticks - waited_ticks);
                    StatsAdd64(g_stats.t_writer_wait, waited_ticks);
                    // (how long the card is then left alone is the pass's own business: LogFiles::DrainDebug, pace::RestAfterPass)
                }
                // The writer is leaving: the lines of the main log that are still waiting go to the card now (2026-11-10). When the
                // writer was in a long write at the end of the launch, Shutdown's FlushWithin gave up and put its lines back in the
                // queue, and nobody wrote them: 'shutdown: done' could be missing from the log of a clean exit.
                if (g_debug_level > 0) { // (log off: no queue and no file, nothing is touched)
                    const SwapGateScope gate(kGateWriter);
                    log_files_->Flush();
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
            const SwapGateScope gate(kGateTrail);
            char stamp[24], started[24], ids[48];
            FormatConsoleClock(osGetTime(), stamp);
            FormatConsoleClock(start_os_ms_, started);
            // (which launch this is - the file holds the trail of many - and when its plugin started: a launch that ended in a crash is
            // told from the next one by these, not by the order of the lines alone)
            LineBuf id_line(ids, sizeof(ids) - 1);
            id_line.Str("title ").Hex(static_cast<u32>(game_id_ >> 32), 8).Hex(static_cast<u32>(game_id_), 8).Str(" pid 0x").Hex(process_id_);
            ids[id_line.size()] = 0;
            lines.insert(lines.begin(), std::string("sleep: the trail of this launch begins | ") + ids + " | plugin started at console clock " + started +
                                            " | tick " + std::to_string(static_cast<u32>(platform::NowMs())) + " ms | written at console clock " +
                                            stamp + " | AzaharLink " + kVersion);
            log_files_->WriteCrumbs(lines.data(), lines.size()); // (with what happened before this turn: one write and one flush)
            crumb_written_.store(crumbs_.taken());
        }
        // The stop (RequestStop at the framework's EXIT, then Shutdown) gets one more turn: what was noted up to it - the EXIT of the process above all,
        // pushed just before RequestStop - is written before the thread leaves.
        for (bool last = false; !last;) {
            if (!crumb_stop_.load()) {
                const s64 wait_ns = crumbs_.watching() ? static_cast<s64>(SleepCrumbs::kPollMs) * 1000000LL : -1;
                svcWaitSynchronization(crumb_wake_, wait_ns);
            }
            last = crumb_stop_.load();
            const SwapGateScope gate(kGateTrail); // (its lines go to the card: azl/swap_gate.hpp)
            lines.clear();
            crumbs_.Poll(static_cast<u32>(platform::NowMs()), osGetTime(), &lines);
            log_files_->WriteCrumbs(lines.data(), lines.size()); // (the lines of the turn in one write and one flush, none when there are none)
            crumb_written_.store(crumbs_.taken());
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
// Debug-independent: even with the log off, one synchronous line and an OSD notice when a closed game is still in memory. A
// closed game that was never finalized (Monster Hunter XX after a chat session, console 63) keeps its commit in the APPLICATION
// limit's books AND its pages in the region, so this game's own figure must come from the process (svcGetProcessInfo type 2,
// as 3DS Link's heap_plan), never from the shared limit: the limit's current already carries the zombie, and a comparison
// against it cancels the zombie on both sides. The decision is zombie::StartupForeign (PC-tested): the books more than own +
// 4 MB, or the region more than own + the plugin's block + 4 MB. Monster Hunter XX panics in crt0 at 0x2E5E94 before a frame,
// so the synchronous log line is the deliverable; the OSD notice may never show. Skipped on an emulator (its books and its
// process figures are its own model; a closed game left in memory is the console's kernel). Five official svcs, at most one
// synchronous write.
void Plugin::CheckStartupOccupancy() {
    if (CTRPluginFramework::System::IsCitra()) {
        return;
    }
    s64 used = 0;
    svcGetSystemInfo(&used, 0, 1); // APPLICATION region in use
    // The limit's current COMMIT (the books: every APPLICATION process's commit, a zombie's included)...
    s64 books = 0;
    {
        ResourceLimitType name = RESLIMIT_COMMIT;
        Handle limit = 0;
        if (R_SUCCEEDED(svcGetResourceLimit(&limit, CUR_PROCESS_HANDLE))) {
            svcGetResourceLimitCurrentValues(&books, limit, &name, 1);
            svcCloseHandle(limit);
        }
    }
    // ...and this game's own, read from its process before its crt0.
    s64 own = 0;
    if (R_FAILED(svcGetProcessInfo(&own, CUR_PROCESS_HANDLE, 2))) {
        own = 0;
    }
    s64 plugin_block = 0;
    const CTRPluginFramework::PluginHeader* header = CTRPluginFramework::FwkSettings::Header;
    if (header) {
        plugin_block = static_cast<s64>(header->exeSize) + static_cast<s64>(header->heapSize);
    }
    const u32 foreign = zombie::StartupForeign(books, used, own, plugin_block);
    if (foreign == 0) {
        return; // a fresh launch: the books and the region hold no more than this game (plus the plugin) and a margin
    }
    const unsigned long used_kb = static_cast<unsigned long>(used / 1024);
    const unsigned long books_kb = static_cast<unsigned long>(books / 1024);
    const unsigned long own_kb = static_cast<unsigned long>(own / 1024);
    const unsigned long plugin_kb = static_cast<unsigned long>(plugin_block / 1024);
    char line[320];
    char wall[24];
    FormatConsoleClock(osGetTime(), wall);
    const int n = std::snprintf(line, sizeof(line),
        "[%s] startup: a closed game is still in memory (foreign %lu: books %lu KB, region used %lu KB, own %lu KB, plugin %lu KB, "
        "margin 4096 KB), restart the console\n",
        wall, static_cast<unsigned long>(foreign), books_kb, used_kb, own_kb, plugin_kb);
    if (n > 0 && n < static_cast<int>(sizeof(line))) {
        MakeDataDir();
        File out;
        if (File::Open(out, kLogSyncPath, File::RWC | File::APPEND | File::SYNC) == File::SUCCESS) {
            out.Write(line, static_cast<u32>(n));
        }
    }
    PostNotification("A closed game is still in memory: restart the console before starting another game");
}

// The synchronous line goes to the main log's file (kLogSyncPath), never to AzaharLink.mem (a table that is read with sscanf and written over at every launch).
// What the user sees and what decides the next step (REGISTRO, phase N): "try 1" is the normal case and has no line here (the log has it, with debug=1);
// a later try = the port was busy for a moment (0xD0401834) or the kernel had no session left (0xC8601809); NOT connected = the plugin stayed off.
bool Plugin::CheckPluginLoaderLink() {
    const plgguard::State& s = g_plg_guard;
    char wall[24];
    FormatConsoleClock(osGetTime(), wall);
    const int head = std::snprintf(g_plg_line, sizeof(g_plg_line), "[%s] ", wall);
    const int body = head > 0 && head < static_cast<int>(sizeof(g_plg_line)) - 2
                         ? plgguard::FormatLine(s, g_plg_line + head, sizeof(g_plg_line) - static_cast<std::size_t>(head) - 1)
                         : 0;
    if (body <= 0) {
        return s.connected;
    }
    g_plg_line_body = head;
    if (plgguard::WorthALine(s)) {
        const int n = head + body;
        g_plg_line[n] = '\n';
        MakeDataDir();
        File out;
        if (File::Open(out, kLogSyncPath, File::RWC | File::APPEND | File::SYNC) == File::SUCCESS) {
            out.Write(g_plg_line, static_cast<u32>(n + 1));
        }
        g_plg_line[n] = '\0';
    }
    return s.connected;
}

void Plugin::CompareMemoryWithLastLaunch() {
    CheckStartupOccupancy(); // debug-independent: one synchronous line + an OSD notice if a closed game is still in memory
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
    if (active_ || startup_issue_.empty() || startup_issue_once_) {
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
    CheckDebugLoad(s, dt);
    CheckLinkSilence(s, dt);
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
    {
        // The flow of the room's stream (2026-11-14; a line of its own, in every window - a zero is an answer too: azl/flow_relief.hpp): how late the
        // room's packets reached the game, how near the waiting data came to ENet's limit and what the limit did, and what the relief of the listed
        // titles did. (About 440 characters with ten digits in every number; longer than a line of Log() may be: straight to the queue.)
        char flow_line[448];
        flow::FormatStatsLine(s, flow::IsDenseTitle(rules_id_), flow_line, sizeof(flow_line));
        log_files_->Append(flow_line);
        // (and, in the same buffer, what was lost on the way and what the bound of the placeholders refused: the line "room stream losses")
        flow::FormatLossLine(s, title_rules::RulesFor(rules_id_).cap_drop_max, flow_line, sizeof(flow_line));
        log_files_->Append(flow_line);
    }
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
    char text[1536]; // (2m-diag: 1100 until then; with ten digits in every number and every clause the line is about 1500 characters)
    int n = 0;
    Append(text, sizeof(text), &n,
           "stats: ms per second, CPU time of our code (waiting is not counted) | plugin total %lu.%lu (CPU) = %lu.%lu%% of a core",
           static_cast<unsigned long>(c.plugin10) / 10, static_cast<unsigned long>(c.plugin10) % 10,
           static_cast<unsigned long>(c.plugin10) / 100, static_cast<unsigned long>(c.plugin10) / 10 % 10);
    const u64 card_ms = azl_ticks_to_ms(s.t_writer_wait);
    const StretchBy stretched_by = WhoStretchedTheWindow(dt_ms, kStatsIntervalMs, card_ms, s.writer_late_max, log_thread_.running());
    if (stretched_by != StretchBy::None) {
        // (2m-diag D3) a window twice the nominal one or more. (2m-rev) Who held it (the writer in the SD card or waking late, the caller of
        // Tick with no writer thread, or something else), and what it does to the figures: those per second are averages over the real
        // length and stay right, the counts of the window (frames, rounds longer than 50 ms) are those of all of it. (The SD card and the
        // writer's late wake-ups are in the line already, in "SD card" and "log writer ... late": the line stays within its 1536 bytes.)
        Append(text, sizeof(text), &n, " | WINDOW STRETCHED %s: %lu ms for %lu, per second: averages; counts: all of it",
               StretchedByText(stretched_by), static_cast<unsigned long>(dt_ms), static_cast<unsigned long>(kStatsIntervalMs));
    }
    Append(text, sizeof(text), &n,
           " | net thread %lu.%lu: enet %lu.%lu, frames+engine %lu.%lu, queue+tick %lu.%lu",
           static_cast<unsigned long>(c.net10) / 10, static_cast<unsigned long>(c.net10) % 10,
           static_cast<unsigned long>(c.enet10) / 10, static_cast<unsigned long>(c.enet10) % 10,
           static_cast<unsigned long>(c.frames10) / 10, static_cast<unsigned long>(c.frames10) % 10,
           static_cast<unsigned long>(c.queue10) / 10, static_cast<unsigned long>(c.queue10) % 10);
    Append(text, sizeof(text), &n, " | game threads: uds %lu.%lu (%lu calls), %lu other ipc calls, frame callback %lu.%lu, chat osd %lu.%lu (%lu, max %lu us)",
           static_cast<unsigned long>(c.uds10) / 10, static_cast<unsigned long>(c.uds10) % 10,
           static_cast<unsigned long>(s.ipc_requests), static_cast<unsigned long>(s.pass_calls),
           static_cast<unsigned long>(c.frame10) / 10, static_cast<unsigned long>(c.frame10) % 10,
           static_cast<unsigned long>(c.chat10) / 10, static_cast<unsigned long>(c.chat10) % 10,
           static_cast<unsigned long>(s.chat_osd_draws), static_cast<unsigned long>(s.chat_osd_max_ticks / 268));
    Append(text, sizeof(text), &n, " | log writer %lu.%lu wall, not CPU, not in the total (%lu wake-ups, late avg %lu max %lu ms, %lu lines dropped since the start: the queue was full)",
           static_cast<unsigned long>(c.writer10) / 10, static_cast<unsigned long>(c.writer10) % 10,
           static_cast<unsigned long>(s.writer_wakes),
           static_cast<unsigned long>(s.writer_wakes ? s.writer_late_sum / s.writer_wakes : 0),
           static_cast<unsigned long>(s.writer_late_max), static_cast<unsigned long>(log_files_->queue_full_total()));
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
           " than 50 ms: %lu, %lu more only by the pause after a socket error",
           static_cast<unsigned long>(s.frames), fps10 / 10, fps10 % 10, gap_avg10 / 10, gap_avg10 % 10,
           static_cast<unsigned long>(s.frame_gap_max / AZL_TICKS_PER_MS), static_cast<unsigned long>(s.frame_hitches),
           static_cast<unsigned long>(s.net_slow_rounds), static_cast<unsigned long>(s.net_pause_rounds));
    log_files_->Append(text);
}

// The wire: the soc calls by kind, what one service call drained (= how full the soc ring got), the gaps
// between the datagrams of the room (its holds), the twin frames, and what the engine did with nodes.
void Plugin::LogStatsWire(const Stats& s) {
    static const char* const kSocNames[kSocKinds] = {"recv", "recv-empty", "send", "poll"};
    char text[2304];
    // (2304 since 2026-11-11; it was 1600: with every optional clause in it the fixed text of this line alone is about 1700 characters,
    // and about 2200 with ten digits in every number - the clauses at the end, "beacons as host" the last, were cut or lost in the very
    // windows that have the most to say. On the statistics' thread - the log's writer, or the network thread without one: 32 KB of stack.)
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
    if (s.auth_other_game) {
        // A host of a title that keeps to its game (azl/one_game_host.hpp) left newcomers that play another game unanswered.
        Append(text, sizeof(text), &n, " | newcomers from another game not answered %lu",
               static_cast<unsigned long>(s.auth_other_game));
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
    if (s.beacon_built || s.beacon_reused) {
        // 2026-11-10: a host's beacon is made again only when its network changed (a player in or out, the application
        // data): the rest go out from the copy of the last one (UdsEngine::BeaconFrameLocked). Last, so that the clauses
        // before it keep their place for the scripts that read this line ("host beacons" is the uds line's count of the
        // beacons HEARD from a host: these are the ones we send as one).
        Append(text, sizeof(text), &n, " | beacons as host: %lu built, %lu sent again from the copy",
               static_cast<unsigned long>(s.beacon_built), static_cast<unsigned long>(s.beacon_reused));
    }
    log_files_->Append(text);
    // The datagrams handed to the socket (2026-11-11; a line of its own, and only when there is something to say: the line
    // above keeps its shape). A refused send is a datagram ENet believes sent; a large one that was taken whole and never
    // reached the room was lost after the console. A datagram "of kSocSendBig bytes or more" is a FULL datagram, not "a
    // fragment of a large frame": ENet fills every datagram to its MTU with whatever commands wait, so a round of ten small
    // frames is one too, and the line comes out in games that send no large frame at all. To tell whether a game's large
    // frames left the console, hold the count against the frames above ENet's fragment length that the capture shows going
    // out in the same window - never read it alone.
    if (s.soc_send_refused || s.soc_send_failed || s.soc_send_short || s.soc_send_big) {
        n = 0;
        Append(text, sizeof(text), &n,
               "stats: socket sends: refused %lu (%lu of %lu bytes or more), failed %lu, short %lu | datagrams of %lu bytes"
               " or more taken whole %lu, largest datagram %lu B",
               static_cast<unsigned long>(s.soc_send_refused), static_cast<unsigned long>(s.soc_send_refused_big),
               static_cast<unsigned long>(kSocSendBig), static_cast<unsigned long>(s.soc_send_failed),
               static_cast<unsigned long>(s.soc_send_short), static_cast<unsigned long>(kSocSendBig),
               static_cast<unsigned long>(s.soc_send_big), static_cast<unsigned long>(s.soc_send_max_bytes));
        log_files_->Append(text);
    }
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
    // What the writer asked of the SD card since the last of these lines, call by call, and how the capture kept out of the game's way
    // (2026-11-10; a line of its own: azl/log_files.hpp, CardWindow). Longer than a line of Log() may be: straight to the queue.
    char card[512];
    LogFiles::FormatCardLine(log_files_->TakeCardWindow(), card, sizeof(card));
    log_files_->Append(card);
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
// records the ring dropped. Once a minute at most (Warn), and in the log as every warning. The share of the card is taken over the REAL
// length of the window (`window_ms`, 2026-11-10): the writer closes the window and it is the thread that sits in the card, so a window
// "of 5 s" lasted up to 43 s and the notice said "SD card busy 868%".
void Plugin::CheckDebugLoad(const Stats& s, u64 window_ms) {
    if (g_debug_level <= 0 || !DebugRingReady()) {
        return;
    }
    const u32 card_pct = pace::CardBusyPercent(azl_ticks_to_ms(s.t_writer_wait), window_ms);
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
// (2m-diag D2, 2026-11-21) The traffic is taken in datagrams per second over the REAL length of the window (LinkSilenceWorthANotice: a
// window the log writer held for 17 s passed the old bound of 100 datagrams with a third of the rate), and with the shared page's WiFi
// strength at 3 the notice names the room or the network, not the WiFi (Tetris with eight, console 63: six notices "WiFi", all of them
// with full strength). Same notice otherwise, once a minute at most (Warn), and never with the log off.
void Plugin::CheckLinkSilence(const Stats& s, u64 window_ms) {
    if (g_debug_level <= 0) {
        return;
    }
    const u32 silent = s.gap_hist[2] + s.gap_hist[3] + s.gap_hist[4]; // 150-249, 250-499 and 500+ ms
    // (2m-rev) the lowest WiFi strength of the window that ends here (SampleLinkState, every 50 ms); the next one starts again
    const u32 lowest = link_strength_min_.exchange(0xFFu, std::memory_order_relaxed);
    if (LinkSilenceWorthANotice(s.rx_datagrams, silent, window_ms, kStatsIntervalMs)) {
        const u32 now_strength = *reinterpret_cast<volatile const u8*>(kSharedPageWifiStrength);
        const u8 strength = static_cast<u8>(now_strength < lowest ? now_strength : lowest); // (2m-rev) the window's, this reading included
        char text[96];
        std::snprintf(text, sizeof(text), "AzaharLink debug: room link silent %lu times (150+ ms): %s", static_cast<unsigned long>(silent),
                      LinkSilenceWhere(strength));
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
    // (the framework's keep-alive thread, 4 KB of stack: flag stores and the swap below, nothing that formats or takes a lock of the log)
    if (event >= 0 && event < kProcessEvents) {
        event_ms_[event] = static_cast<u32>(platform::NowMs());
        if (event == kEventExit) {
            // The game is in svcExitProcess and Luma3DS waits for the framework's answer: the gate's page owes the limit its bytes back if
            // LateInit has not settled them yet (a game closed in its first seconds, no heap seen). Once (LateInit, SWAP_ENTER); one svc, no wait.
            SettleGateBooksAtExit();
            // The trail, before the probes (the log on only): the EXIT of the process with Luma3DS's plugin word (0x00000003 expected), written by the
            // trail's thread while the rest of the exit runs (REGISTRO, phase N: which close of a game left Luma3DS's block behind).
            if (g_debug_level > 0) {
                NoteSleepEvent(SleepCrumbs::kFwProcExit, LumaPluginWord());
            }
            // The zombie probe (debug only), on the keep-alive thread, BEFORE RequestStop and Shutdown join the writer: the lines go into the
            // log queue and Shutdown's FlushWithin carries them. Rosalina gives the process 2.5 s to terminate, which covers a few hundred svcs.
            // No blocking wait on the writer is added here. These never run on the Old-3DS SWAP path (that end comes through SWAP_ENTER, below).
            zombie::ForcedResLine("EXIT");
            zombie::GameRefcount("EXIT");
            zombie::ThreadSchedDump("EXIT");
            zombie::GameHandleCounts();
            zombie::MemMapWalk();
        }
        if (event == kEventHomeEnter || event == kEventHomeExit) {
            // The trail (the log on only): HOME_ENTER / HOME_EXIT with Luma3DS's plugin word (0x00010001 / 0x00010002 expected).
            if (g_debug_level > 0) {
                NoteSleepEvent(event == kEventHomeEnter ? SleepCrumbs::kFwProcHomeEnter : SleepCrumbs::kFwProcHomeExit, LumaPluginWord());
            }
            // The forced res line at HOME (New 3DS only: an Old 3DS gets SWAP_ENTER/EXIT instead, where no probe runs): a clean close then has
            // a per-class reference AT the event to compare the stuck one against.
            zombie::ForcedResLine(event == kEventHomeEnter ? "HOME_ENTER" : "HOME_EXIT");
            if (event == kEventHomeEnter && clnt_room_) {
                dlp_hub_->RequestClntLeave();  // (1.1.4, G1: the Download Play application leaves its session at the next Tick: one atomic)
            }
        }
        if (event == kEventSleepEnter || event == kEventSleepExit) {
            // (the trail of the sleep: a few stores and one event signal)
            NoteSleepEvent(event == kEventSleepEnter ? SleepCrumbs::kFwEnter : SleepCrumbs::kFwExit, 0);
        }
        if (event == kEventSwapEnter) {
            // The trail first, on the card: a crash from here to SWAP_EXIT leaves it, and the memory of the regions at each swap shows what the HOME menu has to work with.
            NoteSwapToTrail(SleepCrumbs::kFwSwapEnter, event_count_[event] + 1);
            WaitForTrail(kSwapTrailWaitMs);
            if (clnt_room_) {
                dlp_hub_->RequestClntLeave();  // (1.1.4, G1: an Old 3DS gets the swap instead of HOME: the same leave, before the gate closes)
            }
            // A net for an end with no EXIT event (a process killed, a block that does not come back): a game closed from the HOME menu gets
            // its EXIT all the same (Luma3DS maps the plugin's block back first, then sends it), and the Settle is made there. Here the gate's
            // page owes the limit its bytes back if LateInit has not settled them yet, and the game has its heap. Once (with LateInit and the
            // EXIT); a load at every HOME after that; never a wait for the lock of the books, no line (the writer says it with this event's).
            SettleGateBooksAtSwap();
            // Old 3DS, HOME: Luma3DS is waiting for the framework's answer, which this callback precedes, and frees the plugin's block as soon as it has it.
            // The svc hook's gate closes (only its flag changes: no word of code, Sources/ctr/gate.s) and the game's threads counted in it are waited for, outside
            // every lock: a thread asleep in a real service is not counted (the gate makes that svc itself), and the waits of the plugin's own on a game thread end
            // when the gate closes (the hub's queue is woken for it). The line about it comes with the event's, from the writer.
            const u32 gate_left = CloseGate(kSwapDrainTimeoutMs, dlp_hub_ ? &WakeHubForSwap : nullptr, dlp_hub_.get());
            // Then the plugin's own threads: a service answering a call of theirs with the block gone stops the console (kernel panic, process `socket`). After the hooks: a game's thread in the
            // hook may wait for what a thread stopped at the gate holds.
            CloseSwapGate(kSwapGateTimeoutMs);
            // A game's thread still counted in the hook is a certain crash once the block goes (risk O-B): what is left of the one deadline
            // (ctr/swap_gate.hpp, from Luma3DS's event) is spent waiting for it, reading the count only, instead of answering early.
            if (gate_left != 0) {
                WaitGateDrained();
            }
            NoteSwapAnswered();
            EndSwapDeadline();
        } else if (event == kEventSwapExit) {
            OpenGate(); // (idempotent: OnSwapBack opened it already, as soon as the block was mapped)
            OpenSwapGate();
            NoteSwapToTrail(SleepCrumbs::kFwSwapExit, event_count_[event] + 1);
        }
        // Last: the writer reports an event once, when it sees the count change, and the HOME button has just woken it. Counted before the gate closes, it could print the numbers of the swap before this one.
        event_count_[event] = event_count_[event] + 1;
    }
}

// Old 3DS, the block is back: Luma3DS has mapped it, unlocked the plugin's threads and told the framework (PLG_OK) (__wrap_PLGLDR__Reply, below, after the real reply).
// The gate opens: the game's threads call the plugin again. Nothing of the game's code is written (it was written once, before the game ran), so the order in which Luma3DS
// wakes the threads does not matter and the framework's thread keeps its own priority: a game's thread that wakes first in the gate's svc finds the same instructions it went
// to sleep on (the defect of 1.1.2: the two-word sites rewritten under a thread asleep in their svc, LEGO Batman 2's dumps; 0xD8E007F7 on handle 0).
// Old 3DS: how long Rosalina waits for the framework's answer to an event (SWAP_ENTER above all) is NOT a setting of Luma3DS: it is the u64
// waitForReplyTimeout of the plugin's own header (offset 0x28, Luma3DS plgldr.h), which Rosalina writes when it loads the plugin (10 s,
// file_loader.c) and reads again at every wait (plgloader.c, PLG__WaitForReply). Raising it here changes only this launch's copy, in the
// plugin's memory: nothing on the card, nothing of Luma3DS, and the next game starts again from 10 s. Why: when the answer comes after
// Rosalina's wait, Rosalina locks the threads and frees the block anyway and, at the return, the framework's thread stays stuck in its reply
// (no swap back for the plugin, the gate closed, the room lost, the EXIT never answered: risk O-B, analysis/piano-2026-11-08). Our part ends
// by SWAP_ENTER's one deadline (8 s from Luma3DS's event, ctr/swap_gate.hpp); 15 s leaves the framework's own work after our callback room.
// The price: a plugin really stuck makes the HOME menu (and a close) wait 15 s instead of 10. Written only when the field holds exactly the
// 10 s Luma3DS writes (another Luma3DS, another layout: nothing is touched), then cleaned from the data cache for Rosalina's view of it.
// ---------------------------------------------------------------------------------------------
// The exit guard (azl/exit_guard.hpp; Sources/ctr/exit_tramp.s; ctr/hook.cpp, InstallExitGuard). It runs on the game's thread that called
// svcExitProcess, but on the stack below (the trampoline switches to it): not the game's, whose size nobody knows at that point. One thread
// only (the trampoline's state word), once per launch. New 3DS only: the plugin's block is mapped for good there.
// ---------------------------------------------------------------------------------------------
namespace {

alignas(8) u8 g_exit_guard_stack[8192];
ExitGuardStats g_exit_stats;
char g_luma_line[320]; // (the lines of the plugin's own thread: the install line, the watch of the word; the guard writes none)
constexpr u32 kPluginHeaderEventPtr = 0x18; // PluginHeader.plgldrEvent (Luma3DS plgldr.h): Rosalina's event word, through its alias
constexpr u32 kPluginHeaderReplyPtr = 0x1C; // PluginHeader.plgldrReply: Rosalina's reply word (PLG_WAIT while it waits for the plugin)

volatile s32* RosalinaWord(u32 offset) {
    const CTRPluginFramework::PluginHeader* header = CTRPluginFramework::FwkSettings::Header;
    if (header == nullptr) {
        return nullptr;
    }
    const u32 ptr = *reinterpret_cast<const volatile u32*>(reinterpret_cast<uintptr_t>(header) + offset);
    return ptr >= 0x80000000u && ptr < 0xB0000000u ? reinterpret_cast<volatile s32*>(ptr) : nullptr; // (an alias, PA | 0x80000000: else unknown)
}

void AppendLumaLine(const char* line, int n) {
    if (n <= 0 || n >= static_cast<int>(sizeof(g_luma_line))) {
        return;
    }
    MakeDataDir();
    File out;
    if (File::Open(out, kLumaLogPath, File::RWC | File::APPEND | File::SYNC) == File::SUCCESS) {
        out.Write(line, static_cast<u32>(n));
    }
}


} // namespace

extern "C" void __real_PLGLDR__Reply(s32 event); // (the framework's own reply: the guard answers a stuck HOME event with it)
// The exit guard itself (azl/exit_guard.hpp, Guard): on the exiting game's thread, on g_exit_guard_stack. No file, no heap, no lock of ours: the
// trail's ring (lock-free) and its thread put the lines on the card, waited for kTrailWaitMs at most; with the log off nothing is written and the
// guard still acts.
void Plugin::RunExitGuard() {
    using namespace exitguard;
    volatile u32* const word = LumaWordAliasForWrite();
    if (word == nullptr) {
        return;
    }
    volatile s32* const reply = RosalinaWord(kPluginHeaderReplyPtr);
    volatile s32* const event = RosalinaWord(kPluginHeaderEventPtr);
    NoteSleepEvent(SleepCrumbs::kExitGuardEnter, *word); // (first, before any wait: a guard that never ends is seen too)
    Guard([word]() { return *word; }, [reply]() { return reply != nullptr ? *reply : 0; }, [event]() { return event != nullptr ? *event : 0; },
          []() { svcSleepThread(static_cast<s64>(kWaitStepMs) * 1000000LL); },
          [](s32 ev) { __real_PLGLDR__Reply(ev); },
          [](const Seen& s) {
              NoteSleepEvent(SleepCrumbs::kExitGuardRosalina,
                             (static_cast<u32>(static_cast<u16>(s.reply_first)) << 16) | static_cast<u16>(s.event_first));
              NoteSleepEvent(SleepCrumbs::kExitGuardWaited, s.waited_ms | (s.replied_for_plugin ? 0x80000000u : 0u));
              NoteSleepEvent(SleepCrumbs::kExitGuardWordLast, s.word_last);
              NoteSleepEvent(SleepCrumbs::kExitGuardAction, static_cast<u32>(s.action));
              Get().WaitForTrail(kTrailWaitMs); // (the trail on the card, kTrailWaitMs at most; no log: at once)
          },
          [word]() {
              *word = kStatusRunning; // (the svc right after this raises EXIT on it; Rosalina frees the block)
              __sync_synchronize();
          });
}

void Plugin::WatchLumaWord(u64 now_ms) {
    if (!luma_alias_) {
        return;
    }
    const u32 w = LumaPluginWord();
    if (w == luma_watch_last_) {
        return;
    }
    const u32 before = luma_watch_last_;
    luma_watch_last_ = w;
    const unsigned long since = static_cast<unsigned long>(now_ms - start_ms_);
    if (g_debug_level > 0 && luma_watch_lines_ < kLumaWatchLines) {
        ++luma_watch_lines_;
        Log("luma: plugin word 0x%08lx -> 0x%08lx at %lu.%03lu s (the uncached alias)", static_cast<unsigned long>(before),
            static_cast<unsigned long>(w), since / 1000, since % 1000);
    }
    if ((w & exitguard::kStatusMask) == exitguard::kStatusNone && !luma_lost_noted_) {
        luma_lost_noted_ = true;
        char wall[24];
        FormatConsoleClock(osGetTime(), wall);
        const int n = std::snprintf(g_luma_line, sizeof(g_luma_line),
            "[%s] luma: title %016llx pid 0x%lx: the plugin word is 0x%08lx (state 0: Rosalina has no plugin for this game) %lu.%03lu s after the "
            "start, word before 0x%08lx: no HOME or EXIT event will come; the exit guard sets it to running at the exit\n",
            wall, static_cast<unsigned long long>(CTRPluginFramework::Process::GetTitleID()), static_cast<unsigned long>(process_id_), static_cast<unsigned long>(w), since / 1000,
            since % 1000, static_cast<unsigned long>(before));
        AppendLumaLine(g_luma_line, n);
        Log("luma: the plugin word is 0 with the plugin running (%lu.%03lu s): Rosalina lost this game", since / 1000, since % 1000);
    }
}

void Plugin::RaiseRosalinaReplyWait() {
    const CTRPluginFramework::PluginHeader* header = CTRPluginFramework::FwkSettings::Header;
    if (header == nullptr) {
        return;
    }
    volatile u64* wait_ns = reinterpret_cast<volatile u64*>(reinterpret_cast<uintptr_t>(header) + kRosalinaReplyWaitOffset);
    if (*wait_ns != kRosalinaReplyWaitNs) {
        return;
    }
    *wait_ns = kRosalinaReplyWaitRaisedNs;
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, reinterpret_cast<u32>(wait_ns), sizeof(u64));
    rosalina_wait_raised_ = *wait_ns == kRosalinaReplyWaitRaisedNs;
    SetSwapDeadlineRaised(rosalina_wait_raised_); // (SWAP_ENTER's deadline follows Luma3DS's wait)
}

void Plugin::OnSwapBack() {
    const u64 start = platform::NowUs();
    OpenGate();
    const u32 took_us = static_cast<u32>(platform::NowUs() - start);
    OpenSwapGate();
    NoteSleepEvent(SleepCrumbs::kFwSwapBack, took_us);
}

void Plugin::NoteSwapToTrail(u32 kind, u32 number) {
    NoteSleepEvent(kind, number);
    if (g_debug_level > 0) {
        NoteSleepEvent(SleepCrumbs::kMemSystem, SleepCrumbs::PackRegion(osGetMemRegionUsed(MEMREGION_SYSTEM), osGetMemRegionSize(MEMREGION_SYSTEM)));
        NoteSleepEvent(SleepCrumbs::kMemApplication, SleepCrumbs::PackRegion(osGetMemRegionUsed(MEMREGION_APPLICATION), osGetMemRegionSize(MEMREGION_APPLICATION)));
        NoteSleepEvent(SleepCrumbs::kMemBase, SleepCrumbs::PackRegion(osGetMemRegionUsed(MEMREGION_BASE), osGetMemRegionSize(MEMREGION_BASE)));
    }
}

void Plugin::WaitForTrail(u32 timeout_ms) {
    if (g_debug_level <= 0 || crumb_wake_ == 0) {
        return;
    }
    const u32 budget_ms = SwapClampMs(timeout_ms); // (SWAP_ENTER's one deadline, ctr/swap_gate.hpp; no deadline: the budget as given)
    const u32 asked_ms = timeout_ms;
    timeout_ms = budget_ms;
    const u32 target = crumbs_.pushed();
    for (u32 waited = 0; waited < timeout_ms && static_cast<s32>(crumb_written_.load() - target) < 0; waited += 2) {
        svcSleepThread(2000000LL);
    }
    if (budget_ms < asked_ms && static_cast<s32>(crumb_written_.load() - target) < 0) {
        NoteSwapWaitCut(2); // (the deadline ended it with the trail not written)
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
                      static_cast<int>(Process::Event::SLEEP_EXIT) == kEventSleepExit &&
                      static_cast<int>(Process::Event::SWAP_ENTER) == kEventSwapEnter &&
                      static_cast<int>(Process::Event::SWAP_EXIT) == kEventSwapExit,
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
        if (i == kEventSwapEnter || i == kEventSwapExit) {
            const GateStats swap = GetGateStats();
            if (i == kEventSwapEnter) {
                LogGateBooksAtEvents(); // (what SWAP_ENTER did with the books, if anything)
                const SwapDeadlineStats& deadline = LastSwapDeadline();
                Log("swap: our part answered %lu ms after %s (anchor %lu ms old; deadline %lu ms; Luma3DS waits %s) | waits cut by the deadline: %s%s%s%s%s%s",
                    static_cast<unsigned long>(deadline.answered_ms),
                    deadline.anchored == 1 ? "Luma3DS's event" : deadline.anchored == 2 ? "an older anchor (the earlier deadline)" : "the callback (no fresh anchor)",
                    static_cast<unsigned long>(deadline.anchor_age_ms), static_cast<unsigned long>(deadline.deadline_ms),
                    rosalina_wait_raised_ ? "15 s, raised" : "10 s", deadline.cut == 0 ? "none" : "",
                    (deadline.cut & 1) ? "mirror " : "", (deadline.cut & 2) ? "trail " : "", (deadline.cut & 4) ? "hooks " : "",
                    (deadline.cut & 8) ? "plugin's threads " : "", (deadline.cut & 16) ? "second drain (a game's thread was still in the hook)" : "");
                const SwapGateStats& gate = g_swap_gate.stats;
                // (chat: the game's thread in the chat's OSD callback, which never stops at the gate: it skips its frame once the gate is closed)
                Log("swap gate: in a service call at the start: network %lu, log writer %lu, trail %lu, chat %lu | left: network %lu, log writer %lu, trail %lu, chat %lu | waited %lu us | %lu stop(s) at the gate so far",
                    static_cast<unsigned long>(gate.inside_start / kGateNet & 0xFF), static_cast<unsigned long>(gate.inside_start / kGateWriter & 0xFF),
                    static_cast<unsigned long>(gate.inside_start / kGateTrail & 0xFF), static_cast<unsigned long>(gate.inside_start / kGateOsd & 0xFF),
                    static_cast<unsigned long>(gate.inside_left / kGateNet & 0xFF),
                    static_cast<unsigned long>(gate.inside_left / kGateWriter & 0xFF), static_cast<unsigned long>(gate.inside_left / kGateTrail & 0xFF),
                    static_cast<unsigned long>(gate.inside_left / kGateOsd & 0xFF),
                    static_cast<unsigned long>(gate.waited_us), static_cast<unsigned long>(g_swap_gate.stops));
                Log("swap: gate closed, %lu in flight at the start, %lu left after %lu us | post skipped %lu so far | "
                    "followed sessions closed unseen %lu, APT slots reused %lu, sessions not followed %lu",
                    static_cast<unsigned long>(swap.gate.in_flight_start), static_cast<unsigned long>(swap.gate.in_flight_left),
                    static_cast<unsigned long>(swap.gate.waited_us), static_cast<unsigned long>(swap.gate.post_skipped),
                    static_cast<unsigned long>(swap.traced_stale), static_cast<unsigned long>(swap.traced_evicted),
                    static_cast<unsigned long>(swap.traced_dropped));
                if (swap.gate.in_flight_left != 0) {
                    // (the block went with a game's thread still counted in the plugin's code: what the gate knows of the last one that came in)
                    Log("swap: gate drain timeout: %lu thread(s) still in the plugin; the last one in came from the stub at 0x%08lx (call site 0x%08lx), handle 0x%08lx",
                        static_cast<unsigned long>(swap.gate.in_flight_left), static_cast<unsigned long>(swap.gate.last_site),
                        static_cast<unsigned long>(swap.last_site_address), static_cast<unsigned long>(swap.gate.last_handle));
                }
            } else {
                Log("swap: gate open (%lu close(s), %lu open(s) so far, post skipped %lu) | fake sessions: %lu in the gate's table, "
                    "%lu call(s) answered by the gate while shut, %lu closed while shut and forgotten, %lu not in the table (full)",
                    static_cast<unsigned long>(swap.gate.closes), static_cast<unsigned long>(swap.gate.opens),
                    static_cast<unsigned long>(swap.gate.post_skipped), static_cast<unsigned long>(swap.fake_in_table),
                    static_cast<unsigned long>(swap.fake_hits), static_cast<unsigned long>(swap.fake_settled),
                    static_cast<unsigned long>(swap.fake_full));
            }
        }
    }
}

void Plugin::SampleLinkState(u64 now_ms) {
    if (now_ms < next_link_sample_ms_) {
        return;
    }
    next_link_sample_ms_ = now_ms + kLinkSampleIntervalMs;
    const u8 strength = *reinterpret_cast<volatile const u8*>(kSharedPageWifiStrength);
    const u8 state = *reinterpret_cast<volatile const u8*>(kSharedPageWifiState);
    if (strength < link_strength_min_.load(std::memory_order_relaxed)) {
        link_strength_min_.store(strength, std::memory_order_relaxed); // (2m-rev) the window's lowest, for CheckLinkSilence
    }
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
    start_os_ms_ = osGetTime(); // (the trail's first line: when this launch's plugin started, by the console's clock)
    if (R_FAILED(svcGetProcessId(&process_id_, CUR_PROCESS_HANDLE))) {
        process_id_ = 0;
    }
    file_layer_.reset(new CtrFiles());
    MakeDataDir(); // (the framework opens the SD card before PatchProcess: the folder can be made here)
    bootdiag::Begin(); // (the diagnostic build only: the state of the process before we touch anything, on the card at once)
    log_files_.reset(new LogFiles(file_layer_.get(), kDataDir, start_ms_));
    log_files_->SetConsoleClock(&ConsoleClockMs); // (the first line of every capture file)
    log_files_->SetStopCapture(&g_swap_gate.closed); // (the writer leaves the card when an Old 3DS is about to swap the block out)
    log_files_->SetGameReads(&g_game_reads_begun, &g_game_reads_done, &GatePostsSkipped); // (... and keeps off it while the game reads its files: the hook counts them, log on only)
    SetLogSink([this](const char* line) { log_files_->Append(line); });

    // The settings come first. Two of them have to be known BEFORE the game runs and before the first line is
    // logged: whether the plugin is on at all (the hooks cannot be taken out later without breaking sessions the
    // game already holds: it opens its ndm:u session in the first seconds) and whether anything is logged
    // (debug=0: Log() returns at once, no file is created and no ring is allocated). The framework opens the SD card
    // before calling PatchProcess. If the settings cannot be read, the plugin stays ON and the log stays off.
    const bool config_read_early = LoadConfig();
    config_read_early_ = config_read_early;
    // The framework's connection to Luma3DS's plugin loader (azl/plg_guard.hpp), made before this: without it no event of the process (HOME, sleep, swap,
    // exit) reaches the plugin, and an Old 3DS would swap the block out under hooks and threads nobody stopped. The plugin then stays out of this launch
    // as when it is switched off (Sources/main.cpp, !active()): no hooks, no network, no chat, no mirror, and the reason on screen once.
    const bool loader_linked = CheckPluginLoaderLink();
    active_ = config_.enabled && config_.tunnel_enabled && loader_linked;
    // 1.1.4 (G1, K5): the system's Download Play application takes the room's Download Play only when the plugin joins a room by itself; else the
    // plugin stays out of it - no hooks: ndm:u and dlp:CLNT are the system's, local Download Play and 3DS Link Delivery work (ClntStaysLocal).
    if (active_ && ClntStaysLocal(CTRPluginFramework::Process::GetTitleID(), config_read_early, !config_.host.empty(), config_.autoconnect, menu_off())) {
        active_ = false;
        clnt_local_ = true;
    }
    if (!loader_linked) {
        startup_issue_ = "3DS Link: the plugin could not talk to Luma's plugin loader: restart the game";
        startup_issue_once_ = true;
        PostNotification(startup_issue_);
    }
    {
        s64 is_new = 0;
        swap_console_ = svcGetSystemInfo(&is_new, 0x10001, 0) != 0; // (Luma3DS swaps the plugin's block out on an Old 3DS: the very test of thread_core)
    }
    if (swap_console_ && loader_linked) {
        RaiseRosalinaReplyWait();
    }
    if (!swap_console_ && loader_linked) {
        luma_alias_ = SetupLumaWordAlias(); // (a New 3DS with Luma3DS: the probes and the exit guard read the word as Luma writes it)
    }
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
        if (g_plg_line_body >= 0) {
            Log("%s", g_plg_line + g_plg_line_body);
        }
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
    // 1.1.4 (G4, contract K8): a title of the band of the prepared children is the Download Play child the app prepared when the index has its row: it
    // runs with the rules and the name of that row's game (the room is still told this process's own id, 2-G3.md D-f). No row: as any title.
    rules_id_ = game_id_;
    if (title_rules::PreparedSlotOfProcess(game_id_) != 0) { // (the build 2h: 00040010|low or 00040000|low of the band)
        NotePreparedChild();
    }
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
        // Which kernel this log is about (a report is read against that version), and the hardware itself: a New 3DS can show the Old layout.
        s64 luma = 0;
        s64 is_new = 0;
        svcGetSystemInfo(&luma, 0x10000, 0);
        const bool new_hardware = svcGetSystemInfo(&is_new, 0x10001, 0) == 0;
        Log("console: %s hardware, kernel 0x%08lx, firm 0x%08lx, Luma3DS 0x%08lx", new_hardware ? "New 3DS" : "Old 3DS",
            static_cast<unsigned long>(osGetKernelVersion()), static_cast<unsigned long>(osGetFirmVersion()), static_cast<unsigned long>(luma));
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
    // (ctr/thread_core.hpp: on an Old 3DS a worker that has not begun would escape the stop of the plugin's threads at the swap)
    Log("threads: %lu framework start-up thread(s) moved from core 1 to core 0%s", static_cast<unsigned long>(threadcore::FrameworkThreadsMoved()),
        threadcore::MovedForListedTitle() ? " (this title is on the list of games whose own thread the kernel refuses on core 1 while the worker holds it)" : "");
    clock_base_os_ms_ = osGetTime();
    clock_base_ms_ = start_ms_;
    // Before the game's own start-up (this runs ahead of its first instruction): what the plugin
    // block has already cost the game's budget.
    LogMemory("at start");
    CompareMemoryWithLastLaunch();

    Log("config: read at startup: %s, plugin %s", config_read_early ? "yes" : "no (defaults)",
        active_ ? "ON" : !loader_linked ? "OFF (no connection to Luma3DS's plugin loader): no hooks, no network" : "OFF (enabled=0 or tunnel=0): no hooks, no network");
    if (clnt_local_) {
        // (no address, no MAC: the log is shared)
        Log("dlp: the Download Play application with no room to join by itself (the settings %s): the plugin stays out of it, the system's"
            " Download Play (ndm:u, dlp:CLNT) is untouched", config_read_early ? "say so" : "were not read at the start");
        PostNotification("3DS Link: Download Play is local here (no room)");
    }
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
        // memory for them the plugin stays out of the game (the real wireless keeps working). A game that cannot spare the block
        // while it sizes its heap gets it at the first connection (socplan::kLateBlockTitles); an empty file /3ds/3DSLink/soc-from-system
        // sends such a game down the road of an Old 3DS on a New one: the block from the SYSTEM region and on the kernel's books. Since 1.1.2
        // it no longer tries the whole road: on a New 3DS the game holds about 80 MiB of a 124 MiB APPLICATION region, so pages are left and the Charge's
        // reservation finds them and goes the kernel's way ("made through the kernel's reservation"); the fallback written in kernel mode
        // (books::ChargeOrWrite, kNoPages) and the Uncharge over the three regions run only where the game holds the whole region, an Old
        // 3DS with Monster Hunter XX: only there are they tried.
        if (socplan::SharesGameSockets(game_id_)) {
            AllowSocketGuest(); // (a game that opens its own sockets first: the plugin is a guest in them, ctr/soc.cpp)
        }
        const bool late = socplan::TakesBlockLate(game_id_);
        if (late) {
            const bool from_system = File::Exists("/3ds/3DSLink/soc-from-system") == 1;
            LeaveSocketMemoryForLater(from_system);
            Log("soc: this game sizes its heap to the page: the buffer waits for the first connection%s",
                from_system ? " (soc-from-system: from the SYSTEM region)" : "");
        }
        const bool reserved = !bootdiag::Skip("soc") && (late || ReserveSocketMemory());
        LogMemory(late ? "before the socket buffer" : reserved ? "with the socket buffer" : "without a socket buffer");
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
                " served by the plugin, friend code seed %04lx... (cut: the log is shared), %lu bytes of hub",
                static_cast<unsigned long>(game_id_ >> 32), static_cast<unsigned long>(game_id_),
                static_cast<unsigned long>(seed >> 48), static_cast<unsigned long>(sizeof(DlpHub)));
        } else {
            Log("dlp: no memory for the Download Play lobby: dlp:FKCL and dlp:SRVR stay the real services for this game");
        }
    } else if (DlpModeOf(game_id_) == DlpGameMode::RealDlp && active_) {
        // 1.1.3, the real Download Play (Luigi's Mansion): the same hub, made at the same point and never changed, on the adapter that speaks
        // the real Download Play on the wire (azl/dlp_real_net.hpp). Two allocations, the adapter and the hub; the other games have neither.
        dlp_real_.reset(new (std::nothrow) RealDlpNet(engine_.get()));
        if (dlp_real_) {
            dlp_hub_ = MakeRealDlpHub(engine_.get(), &kernel_, game_id_, console_mac_, dlp_real_.get());
        }
        if (dlp_hub_) {
            hle_->SetDlpHub(dlp_hub_.get());
            const u64 seed = Fnv1a64(console_mac_.data(), console_mac_.size());
            Log("dlp: the real Download Play is carried by the room for this game (title id %08lx%08lx): dlp:FKCL and dlp:SRVR are served by"
                " the plugin and speak Azahar's Download Play, friend code seed %04lx... (cut: the log is shared), %lu bytes of hub and %lu of adapter",
                static_cast<unsigned long>(game_id_ >> 32), static_cast<unsigned long>(game_id_),
                static_cast<unsigned long>(seed >> 48), static_cast<unsigned long>(sizeof(DlpHub)),
                static_cast<unsigned long>(sizeof(RealDlpNet)));
        } else {
            dlp_real_.reset();
            Log("dlp: no memory for the real Download Play: dlp:FKCL and dlp:SRVR stay the real services for this game");
        }
    } else if (DlpModeOf(game_id_) == DlpGameMode::SystemClient && active_) {
        // 1.1.4 (G1): dlp:CLNT of the system's Download Play application, served as a client of the real Download Play over the room: the book, the
        // adapter and the hub (about 1.3 KB + 1.8 KB + 536 B, this application only); ndm:u stays the fake one. Never a game: no row says it.
        dlp_book_.reset(new (std::nothrow) DlpClntBook());
        if (dlp_book_) {
            RealDlpNet* const searcher = new (std::nothrow) RealDlpNet(engine_.get());
            dlp_real_.reset(searcher);
        }
        if (dlp_real_) {
            dlp_hub_ = MakeClntHub(engine_.get(), &kernel_, game_id_, console_mac_, dlp_real_.get(), dlp_book_.get());
        }
        if (dlp_hub_) {
            hle_->SetDlpHub(dlp_hub_.get());
            hle_->SetDlpClnt(true);
            clnt_room_ = true;
            Log("dlp: the Download Play application: dlp:CLNT is served by the plugin as a client of the real Download Play over the room (any game's"
                " lobby is listed, nothing is downloaded, no host is joined: the join is the 3DS Link app's), %lu bytes of hub, %lu of adapter, %lu of"
                " book",
                static_cast<unsigned long>(sizeof(DlpHub)), static_cast<unsigned long>(sizeof(RealDlpNet)), static_cast<unsigned long>(sizeof(DlpClntBook)));
            PostNotification("3DS Link: Download Play through the room");
        } else {
            dlp_real_.reset();
            dlp_book_.reset();
            active_ = false;  // (no hooks: the system's own Download Play; the socket block stays reserved, unused, for this launch)
            Log("dlp: no memory for dlp:CLNT: the plugin stays out of the Download Play application");
        }
    }

    if (prepared_child_ && active_) {
        hle_->SetDlpChild(true);  // (1.1.4, G4: dlp:SRVR IsChild answers true - this title only, never with a hub: UdsHle refuses it then; the plugin off: no hooks, no service)
        rejoin_.HoldIdentity(kChildHoldIdMs);  // (1.1.4, route B D-B9: the 3DS Link app that started it may still be listed with this MAC and name)
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
    // (chat=0: no block, no sink - the room's chat packets go where they always went, unread. A file the card could not give yet
    // makes nothing either: the chat's default is off (DPAPP-2j), and the file may say chat=1 - LateInit reads it again and wires the chat then)
    if (config_read_early && config_.chat) {
        WireChat(true);
    }
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
    //
    // The one thing done whether the plugin is on or off (Luma3DS keeps the plugin loaded and the framework runs either way): a game whose GSP
    // wrapper has its svc behind a stub (LEGO Batman 2) makes the framework abort at start unless the bl is written as the svc, here, before
    // GSP::Initialize and before the hooks look for the framework's sites.
    RewriteStubbedGspWrappers();
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
        SetTrailWaiter(&Plugin::WaitForCloseTrail); // (the close command 12: the log on, a New 3DS only)
        if (!bootdiag::Skip("hooks")) {
            hook_stats_ = InstallSvcHooks(hle_.get(), !swap_console_ && luma_alias_);
            if (IsDownloadPlayTitle(CTRPluginFramework::Process::GetTitleID())) {
                // The framework's GSP::Initialize runs right after this function, and the plugin's own log begins five seconds later: an abort there leaves a -1 in
                // Luma3DS's dump and nothing else. This line is on the card before it: the size of the code and how many svc 0x32 the hooks left to the framework.
                // 0x93000 and 1 are the application the guard of GetTextSize was made for (Europe, version 3073, read on a PC); another size, or 0, is another version
                // or the build of another region (the five whose code was not read: the line is what tells them apart from the card).
                char stamp[24];
                FormatConsoleClock(osGetTime(), stamp);
                AppendLumaLine(g_luma_line,
                               std::snprintf(g_luma_line, sizeof(g_luma_line),
                                             "[%s] start: the Download Play application, 0x%lx bytes of code, %lu svc 0x32 left to the framework\n", stamp,
                                             static_cast<unsigned long>(hook_stats_.text_size), static_cast<unsigned long>(hook_stats_.framework_sites)));
            }
            if (!swap_console_ && luma_alias_ && hook_stats_.gate_ready) {
                // The exit guard (REGISTRO, phase Q): a New 3DS only (an Old 3DS swaps the block out, and the guard is in it).
                g_exit_stats = InstallExitGuard([]() { RunExitGuard(); },
                                                reinterpret_cast<u32>(g_exit_guard_stack) + sizeof(g_exit_guard_stack));
                char wall[24];
                FormatConsoleClock(osGetTime(), wall);
                const int n = std::snprintf(g_luma_line, sizeof(g_luma_line),
                    "[%s] exit guard: title %016llx pid 0x%lx: %lu of %lu ARM svc 0x03 site(s) hooked, trampoline at 0x%08lx, %lu Thumb candidates "
                    "(not hooked), Luma's word 0x%08lx at the start (through its alias)%s%s\n",
                    wall, static_cast<unsigned long long>(CTRPluginFramework::Process::GetTitleID()), static_cast<unsigned long>(process_id_),
                    static_cast<unsigned long>(g_exit_stats.hooked), static_cast<unsigned long>(g_exit_stats.sites),
                    static_cast<unsigned long>(g_exit_stats.tramp), static_cast<unsigned long>(g_exit_stats.thumb),
                    static_cast<unsigned long>(LumaPluginWord()), g_exit_stats.why_not != nullptr ? ": no guard, " : "",
                    g_exit_stats.why_not != nullptr ? g_exit_stats.why_not : "");
                AppendLumaLine(g_luma_line, n);
                if (g_debug_level > 0 && n > 0) {
                    Log("%s", g_luma_line + 2 + std::strlen(wall));
                }
            }
        }
    }
    if (prepared_child_) {
        // (after the hooks: what the game's dlp:SRVR says depends on them too)
        Log("dlp: dlp:SRVR of the prepared child is %s", ChildSrvrLine(hle_->dlp_child(), active_, hook_stats_.gate_ready));
        if (JoinsEarly(prepared_child_, active_, !swap_console_, config_read_early, !config_.host.empty(), config_.autoconnect, menu_off())) {
            EarlyConnect();  // (1.1.4, G4: the prepared child only)
        } else {
            Log("room: the prepared Download Play child does not join early (%s)", !active_ ? "the plugin is off"
                : swap_console_ ? "an Old 3DS: the gate of the plugin's threads comes with LateInit, the room at the first frame"
                                : "the settings do not join a room by themselves, or were not read at the start");
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
    if (identity_resolved_) {
        return;  // (1.1.4, G4: the prepared child resolved it in EarlyInit for its early join)
    }
    identity_resolved_ = true;
    // The name set on the console (Settings > Other settings > Profile), as UTF-16 in the config
    // block 0x000A0000. The game normally has access to the config service; if not, Luma can lend
    // a session (see the srvGetServiceHandle wrapper in soc.cpp).
    u8 language = 1;
    if (R_SUCCEEDED(cfguInit())) {
        u16 name[0x1C / 2 + 1] = {};
        const Result rc = CFGU_GetConfigInfoBlk2(0x1C, 0x000A0000, name);
        // The console's friend code seed (config block 0x00090000, 8 bytes: what the game reads for the NodeInfo it gives nwm::UDS). The games that
        // compare it with the seed of the lobby's players (LobbySeedIsConsoles) get it in the lobby instead of the hash of the MAC.
        u64 console_seed = 0;
        const bool wants_seed = dlp_hub_ && LobbySeedIsConsoles(game_id_);
        const Result seed_rc = wants_seed ? CFGU_GetConfigInfoBlk2(sizeof(console_seed), 0x00090000, &console_seed) : 0;
        CFGU_GetSystemLanguage(&language);
        cfguExit();
        if (wants_seed) {
            if (R_SUCCEEDED(seed_rc) && console_seed != 0) {
                dlp_hub_->SetFriendSeed(console_seed);
                Log("dlp: the lobby's friend code seed is the console's own (config block 0x00090000): %04lx... (cut: the log is shared), the one this"
                    " game gives nwm::UDS",
                    static_cast<unsigned long>(console_seed >> 48));
            } else {
                Log("WARNING: cannot read the console's friend code seed (config block 0x00090000, 0x%08lx): the lobby keeps the hash of the MAC and"
                    " this game may not tell its players apart (Mario Tennis Open stops at its character selection)", static_cast<unsigned long>(seed_rc));
            }
        }
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
        if (dlp_hub_ && LobbySeedIsConsoles(game_id_)) {
            Log("WARNING: cannot read the console's friend code seed (no config service): the lobby keeps the hash of the MAC and this game may"
                " not tell its players apart (Mario Tennis Open stops at its character selection)");
        }
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
    {
        platform::LockGuard lock(name_mutex_);
        if (name_source_ == NameSource::Index) {
            // (1.1.4, G4: the prepared child keeps its game's name from the index, what an early join told the room, on every console)
            Log("game: name '%s' from the index of the prepared children (the game of this Download Play child)", game_name_.c_str());
            return;
        }
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
        if (IsDownloadPlayTitle(game_id_)) {
            ProbeDownloadPlayOpens(); // (D-S1: the Download Play application only; the plugin thread, the file system open)
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
        // A host of a title that keeps its network to its game (azl/one_game_host.hpp, the rows of azl/title_rules.hpp) answers no newcomer
        // that plays another game in the room; for every other title the list stays empty.
        std::vector<Mac> other_game;
        if (one_game::HostKeepsToItsGame(game_id_)) {
            for (const room::MemberInfo& member : info.members) {
                if (one_game::PlaysAnotherGame(game_id_, member.game_id)) {
                    other_game.push_back(member.mac);
                }
            }
        }
        engine_->SetOtherGameMembers(other_game);
        // 1.1.4 (H5): a host of the real Download Play that waits for its guests' children is told who is in the system's Download Play
        // application (a guest is awaited only if it is one of them at its final); every other title never sets the list.
        if (dlp_real_ && dlp_real_->child_wait_on()) {
            std::vector<Mac> download_play;
            for (const room::MemberInfo& member : info.members) {
                if (AwaitsChildOf(member.game_id)) {
                    download_play.push_back(member.mac);
                }
            }
            engine_->SetDownloadPlayMembers(download_play);
        }
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
    // Who came and who left, in the chat (the chat on, at every debug level): this runs on the network thread, the ring's one writer.
    // The first list of a join is the baseline (no line for who was already there: the "has N/M players" notice says it).
    if (room_members_known_) {
        if (ChatSink* sink = room_->chat_sink()) {
            const u32 now_ms = static_cast<u32>(platform::NowMs());
            for (const RoomMemberChange& c : RoomMemberChanges(room_members_, now, room_->mac())) {
                sink->OnPresence(c.nickname.data(), c.nickname.size(), c.joined, now_ms);
            }
        }
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

void Plugin::WaitForCloseTrail() {
    Plugin& self = Get();
    if (g_debug_level <= 0 || self.swap_console_ || self.crumb_wake_ == 0) {
        return; // (no log, no trail, or an Old 3DS: its HOME goes through the swap of the block, which has its own budget)
    }
    const u32 target = self.crumbs_.pushed();
    for (u32 waited = 0; waited < kCloseTrailWaitMs && !GateClosing() && static_cast<s32>(self.crumb_written_.load() - target) < 0; waited += 2) {
        svcSleepThread(2000000LL);
    }
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

// The trail as the chat's input mirror asks about it on the plugin's thread (Sources/ctr/input_mirror.cpp, GiveTrailTime; 2026-11-10): the
// mark is what was pushed up to now, and it is on the card once the trail's thread has written that many (crumb_written_, stored after the
// write and the flush of a turn). With the log off, or no trail's thread, there is nothing to wait for.
u32 Plugin::TrailMark() {
    return Get().crumbs_.pushed();
}

bool Plugin::TrailOnCard(u32 mark) {
    Plugin& self = Get();
    if (g_debug_level <= 0 || self.crumb_wake_ == 0) {
        return true;
    }
    return static_cast<s32>(self.crumb_written_.load() - mark) >= 0;
}

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
    InitSwapGate(); // (before the threads that use it)
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
    // The relief of a dense broadcast stream, with the numbers of the title's row of azl/title_rules.hpp and for no other title (2026-11-14,
    // Mario Party: Island Tour): no setting, no menu entry. Every other title leaves the three at their defaults - a packet a round, every
    // frame sent, refused at ENet's limit - and its transport as it was.
    if (flow::IsDenseTitle(rules_id_)) {
        const title_rules::Rules rules = title_rules::RulesFor(rules_id_);
        room_->SetHandOverBurst(rules.hand_over_burst);
        engine_->SetSameFrameWindow(rules.same_frame_ms);
        room_->SetCapDrop(rules.cap_drop_max);
        room_->SetSendBudget(rules.send_budget_bytes);
        Log("flow: dense-broadcast relief on for this title (%s): a broadcast frame equal to the last one is not sent again for %lu ms, up to %lu"
            " packets of the room go to the game in one round, at ENet's limit up to %lu game frames are dropped instead of refused,"
            " a round sends up to %lu bytes of the game's queue (0 = all)",
            title_rules::NameOf(rules_id_), static_cast<unsigned long>(rules.same_frame_ms), static_cast<unsigned long>(rules.hand_over_burst),
            static_cast<unsigned long>(rules.cap_drop_max), static_cast<unsigned long>(rules.send_budget_bytes));
    }
    // (the file as read now: one the card could not give at start-up may switch the chat either way; off allocates nothing)
    if (config_.chat || chat_sink_.load(std::memory_order_relaxed)) {
        WireChat(config_.chat);
    }
    Log("config: %s enabled=%d host='%s' port=%u nickname='%s' tunnel=%d autoconnect=%d radio_like=%d"
        " socket_buffers=%d rcvbuf_kb=%d sleep_stop=%d net_thread_core=%d menu_key=%s",
        loaded ? "loaded" : "no file,", config_.enabled, config_.host.c_str(), config_.port,
        config_.nickname.c_str(), config_.tunnel_enabled, config_.autoconnect, config_.radio_like,
        config_.socket_buffers, config_.rcvbuf_kb, config_.sleep_stop, config_.net_core, config_.menu_key.c_str());
    Log("debug: log ON (%s)", !active_ ? "AzaharLink.log only: no capture while the plugin is OFF"
                              : DebugRingReady() ? "AzaharLink.log and AzaharLink.debug.log"
                                                 : "AzaharLink.log only: no memory for the capture ring");
    if (active_ && DebugRingReady()) {
        // (what whoever reads the capture must know before its first frame: the diet of 1.1.4, azl/stats.hpp)
        Log("debug: the capture keeps %s",
            g_debug_whole ? "every data frame whole and every command (a study build)"
                          : "the first 32 bytes of a game's data frame (cut) and counts the UDS commands a game polls;"
                            " the Download Play's frames are whole");
    }

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
    // The gate's page went on the kernel's books before the game sized its heap (ctr/gate_page.cpp): the limit comes back down now.
    SettleGatePage();

    ResolveIdentity();
    ResolveGameName();
    if (dlp_real_ && dlp_hub_ && dlp_real::AnnouncesTitle(game_id_)) {
        // (every game that has the hub of the real Download Play - Island Tour, Luigi's Mansion, Mario Kart 7, ...: the name, the icon and the
        // sizes its host announces, read here once from its own copy; a game with no hub reads or keeps nothing)
        GiveAnnouncementItsTitle(dlp_real_.get(), game_id_, language_);
    }
    if (clnt_room_) {
        LoadDlpChildren();  // (1.1.4, G1/G2.3: the Download Play application in the room only)
    }
    if (g_debug_level > 0) {
        LogAddressMap();
    }
    Log("identity: nickname '%s' (%s), MAC %s, game name '%s'", EffectiveNickname().c_str(),
        config_.nickname.empty() ? "from the console" : "from the settings",
        MacToString(console_mac_).c_str(), GameName().c_str());

    if (hook_stats_.no_gate != nullptr) {
        // (a console where the gate's page could not be taken: no hooks at all for this launch, never the alias where the SYSTEM page failed, never the old two-word ones; ctr/gate_page.cpp)
        Log("WARNING: the svc hook is off for this launch: %s", hook_stats_.no_gate);
        PostNotification(std::string("AzaharLink: the hook found no place for its gate (") + hook_stats_.no_gate +
                         "): multiplayer will not work in this launch");
    } else if (hook_stats_.send_sync_sites == 0) {
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
    OpenSwapGate(); // (a thread stopped at the gate could never be joined)
    // The lines nobody has reported yet - the EXIT event above all: the writer reports it too if it wakes first, and
    // whichever of the two comes first logs it once - come before 'shutdown: begin', so that the end of a log reads in
    // the order it happened.
    LogProcessEvents();
    LogGateBooksAtEvents(); // (what the EXIT event, or the last SWAP_ENTER, did with the books, if anything)
    Log("shutdown: begin (%lu hook sites, network %s)",
        static_cast<unsigned long>(hook_stats_.send_sync_sites),
        room_ && room_->state() != RoomClient::State::Idle ? "up" : "idle");
    // From here the capture has half a second to empty its ring (a full ring on a slow card takes tens of seconds, the writer is
    // then stuck in the card's writes and the main log's last lines - how the exit went - never reach the file, as on the full
    // card of the Zelda session: log_files.hpp, LogFileLimits::exit_capture_ms).
    log_files_->BeginShutdown();
    // The hooks stay in the game's code on purpose: the game's own threads may still be running
    // through them while it closes, and the gate's page and the plugin stay mapped until the process
    // dies anyway (nothing of the game's code is ever written after the installation). The HLE keeps
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
    Log("shutdown: done (the chat's input mirror at the game's last event: %s)", MirrorLastText(MirrorLastState()));

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

// ---------------------------------------------------------------------------------------------
// The children the app prepared, for the system's Download Play application in the room (1.1.4, G1 and G2.3 with the contract K2/K3)
// ---------------------------------------------------------------------------------------------

/// How the titles of a row are looked for (dlp_children::PresenceBy). The probe of D-S1 has not given its answer yet (no 'dlp probe:' line in the
/// logs of the first session), so the plugin believes the app's index - the app writes it again at every start from what is installed - and asks
/// the file system only for the one file of "ready": the plugin in the prepared title's folder. FileSystem or Am, once D-S1 says the opens work
/// from the application, need the open of ProbeDownloadPlayOpens as a shared function first.
constexpr dlp_children::PresenceBy kChildrenPresence = dlp_children::PresenceBy::Index;
static_assert(kChildrenPresence == dlp_children::PresenceBy::Index, "FileSystem / Am need a title_present (the open of ProbeDownloadPlayOpens, shared)");

/// /luma/plugins/<prepared>/AzaharLink.3gx (the name the app installs, ConfigApp/source/games.hpp; Luma3DS loads the first .3gx of the folder).
/// (the build 2h) the folder of the child's system id (000400100F8...: the process's id, the folder Rosalina loads) or, for the fallback form and
/// the children of the builds before, the key's.
bool ChildPluginPresent(void* /*ctx*/, u64 prepared) {
    char path[64];
    std::snprintf(path, sizeof(path), "/luma/plugins/%016llX/AzaharLink.3gx", static_cast<unsigned long long>(prepared));
    if (File::Exists(path) == 1) {
        return true;
    }
    std::snprintf(path, sizeof(path), "/luma/plugins/%016llX/AzaharLink.3gx", static_cast<unsigned long long>(title_rules::PreparedSystemOf(prepared)));
    return File::Exists(path) == 1;
}

const char* VerdictText(dlp_children::Verdict v) {
    switch (v) {
    case dlp_children::Verdict::Ready: return "ready";
    case dlp_children::Verdict::NoRow: return "no row";
    case dlp_children::Verdict::NoGame: return "no game";
    case dlp_children::Verdict::NotPrepared: return "not prepared";
    case dlp_children::Verdict::NoChildPlugin: return "no plugin in its folder";
    case dlp_children::Verdict::OtherVersion: return "another version";
    case dlp_children::Verdict::Unknown: break;
    }
    return "not known yet";
}

void Plugin::LoadDlpChildren() {
    const u64 began_ms = platform::NowMs();
    dlp_children::Table* table = new (std::nothrow) dlp_children::Table;
    if (table == nullptr) {
        Log("dlp: no memory for the table of the children: every host of the Download Play application is shown as it is and refused");
        return;
    }
    dlp_children::Clear(table);
    std::string text;
    const bool read = ReadDataFile(dlp_children::kIndexPath, dlp_children::kIndexMaxBytes, text);
    table->index_found = read && dlp_children::ParseIndex(text.data(), text.size(), &table->index);
    const dlp_children::Probe probe = {nullptr, nullptr, &ChildPluginPresent};
    dlp_children::JudgeAll(table->index, table->verdict, probe, kChildrenPresence);
    for (std::size_t r = 0; r < table->index.count; ++r) {
        const dlp_children::Row& row = table->index.rows[r];
        Log("dlp: the child %016llx (game %016llx on the %s, prepared %016llx): %s%s", static_cast<unsigned long long>(row.child),
            static_cast<unsigned long long>(row.game), row.where == 'C' ? "card" : "SD", static_cast<unsigned long long>(row.prepared),
            VerdictText(static_cast<dlp_children::Verdict>(table->verdict[r])),
            table->verdict[r] == static_cast<u8>(dlp_children::Verdict::Ready) ? " (its block is read when a host announces it)" : "");
    }
    dlp_children_.store(table, std::memory_order_release);
    dlp_real_->SetChildren(table);  // (from here the adapter's and the book's: never freed, never changed but for the blocks)
    if (!table->index_found) {
        Log("dlp: no index of the children (%s%s): every host is shown not ready (%s) and refused; the 3DS Link app prepares them (it needs"
            " boot9.bin); %lu ms", dlp_children::kIndexPath, read ? ": another version or no header" : ": no file",
            dlp_children::MarkStyle::Text == dlp_children::kMarkStyle ? "NO" : "the cross", static_cast<unsigned long>(platform::NowMs() - began_ms));
    } else {
        Log("dlp: %lu children in the index (%lu lines refused), marks %s, checked by %s; %lu ms", static_cast<unsigned long>(table->index.count),
            static_cast<unsigned long>(table->index.skipped), dlp_children::MarkStyle::Text == dlp_children::kMarkStyle ? "'OK/NO'" : "the two symbols",
            kChildrenPresence == dlp_children::PresenceBy::Index ? "the index and the child's plugin" : "the file system",
            static_cast<unsigned long>(platform::NowMs() - began_ms));
    }
}

void Plugin::ServeDownloadPlayClient() {
    const u32 refusals = dlp_book_->refusals();
    if (refusals != clnt_refusals_seen_) {
        clnt_refusals_seen_ = refusals;
        const dlp_children::Verdict v = dlp_book_->refused_verdict();
        if (v == dlp_children::Verdict::Ready) {
            // (1.1.4, route B: clnt::kSystemAppJoins is false - the 3DS Link app runs the session with the host and starts the prepared child)
            Log("dlp: the Download Play application asked to join a host whose child is ready on this console: refused: the join is 3DS Link's"
                " (route B)");
            PostNotification("3DS Link: join it from the 3DS Link app (Download Play page)");
        } else {
            Log("dlp: the Download Play application asked to join a host whose child is %s on this console: refused (no download through the room)", VerdictText(v));
            PostNotification(v == dlp_children::Verdict::Unknown ? std::string("3DS Link: still reading the prepared games, try again")
                                                                 : std::string("3DS Link: not ready") + dlp_children::TitleLongTail(v));
        }
    }
    dlp_children::Table* table = dlp_children_.load(std::memory_order_acquire);
    if (table == nullptr) {
        return;
    }
    u32 wanted = dlp_book_->TakeWantedBlocks();
    if (wanted == 0) {
        return;
    }
    std::size_t r = 0;
    while ((wanted & (1u << r)) == 0) {
        ++r;
    }
    wanted &= ~(1u << r);
    if (wanted != 0) {
        dlp_book_->WantRows(wanted);  // (one block per round: the others at the next ones)
    }
    if (dlp_children::BlockStateOf(table, r) != dlp_children::BlockState::NotRead) {
        return;
    }
    const dlp_children::Row& row = table->index.rows[r];
    char name[dlp_children::kBlockNameBytes];
    dlp_children::BlockName(row.prepared, name);
    const std::string path = std::string(dlp_children::kGuestsDir) + name;
    u8* block = nullptr;
    File file;
    if (row.prepared != 0 && File::Open(file, path, File::READ) == File::SUCCESS && file.GetSize() == dlp_children::kBlockBytes) {
        block = new (std::nothrow) u8[dlp_children::kBlockBytes];
        if (block != nullptr && file.Read(block, static_cast<u32>(dlp_children::kBlockBytes)) < 0) {
            delete[] block;
            block = nullptr;
        }
    }
    if (block != nullptr && dlp_children::PutBlock(table, r, block)) {
        Log("dlp: the block of the child %016llx read (its sizes 0x%08lx and 0x%08lx)", static_cast<unsigned long long>(row.child),
            static_cast<unsigned long>(dlp_children::BlockTransfer(block)), static_cast<unsigned long>(dlp_children::BlockRequired(block)));
        return;
    }
    delete[] block;
    dlp_children::NoBlock(table, r);
    Log("dlp: the block of the child %016llx is not there whole (%s): it is shown not prepared", static_cast<unsigned long long>(row.child),
        path.c_str());
}

void Plugin::NoteClntSeam() {
    const u64 armed = dlp_book_->armed_prepared();
    const bool pass = dlp_book_->passphrase_ready();
    if (armed == clnt_seam_armed_ && pass == clnt_seam_pass_) {
        return;
    }
    clnt_seam_armed_ = armed;
    clnt_seam_pass_ = pass;
    if (armed == 0) {
        Log("dlp: the Download Play application has no session with a prepared child now");
    } else {
        Log("dlp: the Download Play application joined a host whose child is prepared here (%016llx)%s", static_cast<unsigned long long>(armed),
            pass ? ": the passphrase came (route B: never armed)" : "");
    }
}

void Plugin::NoteRealDlpHost() {
    dlp_real::ChildWait::End end;
    if (dlp_real_->TakeChildWaitEnd(&end)) {
        Log("dlp: the host waited %lu ms in its state 9 for the guests that went to start their child: %u back in the room, %u stayed in it (an"
            " emulator), %u not back", static_cast<unsigned long>(end.waited_ms), static_cast<unsigned>(end.back), static_cast<unsigned>(end.stayed),
            static_cast<unsigned>(end.missing));  // (no MAC, no name: privacy)
    }
    Mac guest;
    while (dlp_real_->TakeContentGuest(&guest)) {
        if (!content_notices_.First(guest)) {
            continue;  // (once per guest for the game's run)
        }
        std::string name;
        {
            platform::LockGuard lock(members_mutex_);
            name = dlp_real::GuestName(room_members_, guest);
        }
        Log("dlp: a guest asked for the content of the child and was ejected (the child never travels through the room): its name is on the screen%s",
            name.empty() ? " (the room does not list it)" : "");
        PostNotification(dlp_real::ContentNoticeText(name));
    }
}

void Plugin::NotePreparedChild() {
    std::string text;
    const bool read = ReadDataFile(dlp_children::kIndexPath, dlp_children::kIndexMaxBytes, text);
    std::unique_ptr<dlp_children::Index> scratch(new (std::nothrow) dlp_children::Index);
    dlp_children::Row row = {};
    prepared_child_ = scratch && PreparedChildRow(game_id_, read ? text.data() : nullptr, text.size(), scratch.get(), &row);
    if (!prepared_child_) {
        Log("dlp: this title is in the band of the prepared children with no row of its own (%s): the plugin runs as for any title it does not know",
            !read ? "no index" : !scratch ? "no memory to read the index" : "not in the index");
        return;
    }
    rules_id_ = row.game;
    const char* const name = title_rules::NameOf(row.game);
    if (name[0] != '\0') {
        platform::LockGuard lock(name_mutex_);
        game_name_ = name;
        name_source_ = NameSource::Index;  // (LateInit keeps it: the child's own icon is not the game's name, and the room was told this one)
    }
    Log("dlp: this title is the Download Play child 3DS Link prepared: the child %016llx of the game %016llx (%s), with that game's rules",
        static_cast<unsigned long long>(row.child), static_cast<unsigned long long>(row.game), name[0] != '\0' ? name : "no row of rules");
    // The 16 bytes of the jump (G3): the loader's words of this launch, taken once and wiped from the header; never in the log.
    const CTRPluginFramework::PluginHeader* header = CTRPluginFramework::FwkSettings::Header;
    if (header == nullptr) {
        Log("dlp: the plugin loader's words for this launch: the framework's header is not available");
        return;
    }
    volatile u32* const words = reinterpret_cast<volatile u32*>(reinterpret_cast<uintptr_t>(header) + kLoaderWordsOffset);
    u32 shelter[kLoaderWords];
    const bool found = TakeLoaderWords(words, shelter);
    if (found) {
        loader_words_.reset(new (std::nothrow) u32[kLoaderWords]);
        if (loader_words_) {
            std::memcpy(loader_words_.get(), shelter, sizeof(shelter));
        }
    }
    volatile u32* const wipe = shelter;  // (the copy on this stack goes too: a store the compiler cannot leave out)
    for (std::size_t i = 0; i < kLoaderWords; ++i) {
        wipe[i] = 0;
    }
    Log("dlp: the plugin loader's words for this launch (the 16 bytes of the jump): %s", !found ? "none"
        : loader_words_ ? "found, kept for the answer to GetWirelessRebootInfo and wiped from the header" : "found and wiped, no memory to keep them");
}

void Plugin::EarlyConnect() {
    // (what LateInit sets before a room starts, with the same values: it sets them again)
    const Config& settings = config_;
    platform::SetPreferredCore(settings.net_core);
    azl_enet_socket_buffers = settings.socket_buffers ? 1 : 0;
    room_->SetFastData(settings.radio_like);
    ResolveIdentity();  // (the nickname of the join; LateInit's call then does nothing)
    const bool started = Connect();
    Log("room: the prepared Download Play child joins the room from the start (%s, %llu ms after the plugin started): the host's game waits 20 s for it",
        started ? "attempt started" : "no attempt: the first frame tries again", static_cast<unsigned long long>(platform::NowMs() - start_ms_));
}

bool Plugin::ReadDataFile(const char* path, std::size_t max_bytes, std::string& out) {
    File file;
    if (File::Open(file, path, File::READ) != File::SUCCESS) {
        return false;
    }
    const u64 size = file.GetSize();
    if (size == 0) {
        return false;
    }
    // (a bigger file is read up to its last whole line within `max_bytes`: the memory the menu takes stays what it is)
    const std::size_t take = size > max_bytes ? max_bytes : static_cast<std::size_t>(size);
    out.assign(take, '\0');
    if (file.Read(&out[0], static_cast<u32>(take)) < 0) {
        out.clear();
        return false;
    }
    if (take < size) {
        const std::size_t last = out.rfind('\n');
        out.resize(last == std::string::npos ? 0 : last + 1);
    }
    return !out.empty();
}

bool Plugin::LoadConfig() {
    // The old place (an app older than this build) is read while the new file is missing, never written (SaveConfig writes the new
    // place; the app moves the file).
    // F7: AzaharLink.ini missing but AzaharLink.ini.bak there: a save (the app's or the plugin's) was cut between its two renames
    // (azl/whole_file.hpp); the .bak is the whole old file. It is only read here: the next save of the plugin (whole::Replace drops the .bak
    // once the new ini is in place: F7-rev) or the app (ReadTextFile puts it back) puts the files in order.
    // F7-rev: an ini or a .bak that is there and cannot be read whole (it does not open, it is over 4096 bytes, the read fails) is never
    // written over: SaveConfig refuses while config_unreadable_ says so (the app's Unreadable, ConfigApp/source/main.cpp). An empty file holds
    // nothing to keep (a cut of 1.1.3): a save writes it. The old place is never written, so its own failures do not count.
    config_unreadable_ = false;
    CtrWhole card;
    const whole::Source source = whole::Choose(card, kConfigPath);
    const bool ours = source != whole::Source::None; // (the file a save of the plugin replaces)
    config_from_old_place_ = source == whole::Source::None && File::Exists(kOldConfigPath) == 1;
    const char* const ini_path = source == whole::Source::Bak ? kConfigBakPath : config_from_old_place_ ? kOldConfigPath : kConfigPath;
    if (source == whole::Source::Bak) {
        Log("config: read from %s (a save was cut before its end)", kConfigBakPath);
    }
    File file;
    if (File::Open(file, ini_path, File::READ) != File::SUCCESS) {
        config_from_old_place_ = false;
        config_unreadable_ = ours;
        return false;
    }
    const u64 size = file.GetSize();
    if (size == 0 || size > 4096) {
        config_unreadable_ = ours && size != 0;
        return false;
    }
    std::string content(static_cast<std::size_t>(size), '\0');
    if (file.Read(&content[0], static_cast<u32>(size)) < 0) {
        config_unreadable_ = ours;
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
                config_.menu_key = ini::MenuKeyFromFile(value); // (a chord, or "off"/"none": see menu_off(); the old default is the new one)
            }
        } else if (key == "game_name") {
            config_.game_name = value;
        } else if (key == "debug") {
            config_.debug = ini::DebugLevel(value); // (a 2 of 0.1.14 and older is a 1: the third level is gone)
        } else if (key == "rcvbuf_kb") {
            config_.rcvbuf_kb = ini::RcvbufKb(value); // (checked where it is used)
        } else if (key == "sleep_stop") {
            config_.sleep_stop = ini::SleepStop(value); // (0, 1, 2: see azl/ini_values.hpp)
        } else if (key == "chat") {
            config_.chat = ini::ParseBool(value, ini::kChatDefault); // (a slip is the default: off since DPAPP-2j, as the app)
        } else if (key == "chat_key") {
            if (!value.empty()) {
                config_.chat_key = value;
            }
        } else if (key == "chat_pos") {
            config_.chat_pos = ini::ParseChatPos(value); // (one of the four words; anything else is the default, bottom-left)
        }
    }
    chat_on_.store(config_.chat, std::memory_order_relaxed);
    chat_pos_.store(config_.chat_pos, std::memory_order_relaxed);
    return true;
}

bool Plugin::SaveConfig() {
    if (config_unreadable_) {
        // F7-rev: the defaults with one key of the menu never take the place of a file that is there and was not read (LoadConfig)
        Log("config: %s is there but could not be read: not written over (the change holds for this launch only)", kConfigPath);
        return false;
    }
    // All 18 keys, always, in the order the Setup app writes them (ini::Render is the one writer of both): a file made by the
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
    fields.chat = config_.chat;
    fields.chat_key = config_.chat_key;
    fields.chat_pos = config_.chat_pos;
    const std::string text = ini::Render(fields);

    // F7: never in place (a cut between TRUNCATE's SetSize(0) and the write left 0 bytes, and the app then wrote the defaults over them):
    // the new text finished in AzaharLink.ini.tmp and read back, the old file aside, the new one in its place (azl/whole_file.hpp)
    MakeDataDir();
    CtrWhole card;
    if (!whole::Replace(card, kConfigPath, text)) {
        Log("config: cannot write %s (the old file is left whole)", kConfigPath);
        return false;
    }
    return true;
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
    SettleGateFakeClosed(); // (the fake sessions the game closed while the gate was shut: one load when there is none)
    WatchLumaWord(platform::NowMs()); // (Luma3DS's plugin word through its alias: a load when it has not changed)
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
    if (dlp_book_) {
        ServeDownloadPlayClient();  // (1.1.4, G1: the Download Play application in the room only; a load when nothing is asked)
        NoteClntSeam();  // (1.1.4, G1/K7: the seam to G3, read on this thread - where G3 jumps - without the hub's lock, room link or not)
    } else if (dlp_real_) {
        NoteRealDlpHost();  // (1.1.4, H5/H3: a game that hosts the real Download Play only; two loads when nothing happened)
    }
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
    case RejoinPolicy::Note::HeldIdentity:
        Log("room: kept this console's MAC and name while the 3DS Link app leaves the room (%lu ms from the launch, held for %lu ms)",
            static_cast<unsigned long>(now - start_ms_), static_cast<unsigned long>(rejoin_.hold_ms()));
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

// The framework's answer to Luma3DS. With 3 (the plugin is ready for the swap of its block, an Old 3DS only: Luma sends that event nowhere else) the call returns
// once the block is back and mapped: the svc hook's gate opens then (Plugin::OnSwapBack), never before (a game's thread would call into a block that is not there).
// No priority games: nothing of the game's code changes, so who runs first does not matter.
constexpr s32 kReplySwap = 3;

extern "C" void __real_PLGLDR__Reply(s32 event);
extern "C" void __wrap_PLGLDR__Reply(s32 event) {
    __real_PLGLDR__Reply(event);
    if (event == kReplySwap) {
        azl::ctr::Plugin::Get().OnSwapBack();
    }
}

// The framework's connection to Luma3DS's plugin loader, guarded (azl/plg_guard.hpp; -Wl,--wrap=plgLdrInit -Wl,--wrap=PLGLDR__FetchEvent in the Makefile:
// KeepThreadMain's one call of each). Both run on the framework's keep-alive thread - plgLdrInit once, before PatchProcess, FetchEvent every 250 ms -
// and are plain C: no Log, no string, no heap (4 KB of stack, Tests/stack_usage.sh). The arbiter: plgLdrInit takes it from word 3 of the answer of the IPC
// 0x90000 (in the ELF: ldr [tls, #0x8c], then a store to its static plgLdrArbiter) and returns the connection's result only; that word is cleared before
// each try, so a try whose IPC did not answer shows 0. A session without its arbiter is closed (plgLdrExit: the count back to 0, the handle closed)
// and tried again. FetchEvent without a connection answers 0, "no event" (KeepThreadMain takes values outside 1..6 back to its wait): never a
// connection tried there (Rosalina serves plg:ldr and the events in one loop: a try while it waits for our reply blocks 10 s, and the event word
// it then reads is the old one). PLGLDR__Reply needs no guard: the framework replies only to an event it fetched.
extern "C" Result __real_plgLdrInit(void);
extern "C" void plgLdrExit(void);
extern "C" Result __wrap_plgLdrInit(void) {
    azl::plgguard::State& s = azl::ctr::g_plg_guard;
    s64 citra = 0;
    s.emulator = svcGetSystemInfo(&citra, 0x20000, 0) == 0; // (Citra's and Azahar's information type: Luma3DS has none such)
    const u64 start = svcGetSystemTick();
    const Result rc = azl::plgguard::Connect(
        &s,
        []() {
            u32* cmd = getThreadCommandBuffer();
            cmd[3] = 0;
            const s32 r = __real_plgLdrInit();
            return azl::plgguard::Try{r, r >= 0 ? cmd[3] : 0u};
        },
        []() { plgLdrExit(); },
        []() { svcSleepThread(static_cast<s64>(azl::plgguard::kRetryMs) * 1000000LL); });
    s.ms = static_cast<u32>((svcGetSystemTick() - start) / (SYSCLOCK_ARM11 / 1000));
    return rc;
}

extern "C" s32 __real_PLGLDR__FetchEvent(void);
extern "C" s32 __wrap_PLGLDR__FetchEvent(void) {
    return azl::plgguard::FetchOrNone(azl::ctr::g_plg_guard, []() { return __real_PLGLDR__FetchEvent(); });
}
