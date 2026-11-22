// The Download Play page of 3DS Link, its pure part (1.1.4, route B: analysis/piano-1.1.4-2026-11-15/specifiche/8-INGRESSO-DA-3DSLINK.md, approved by
// the user on 2026-11-16, section 3.2). The app itself runs the Download Play session with a host of the room (dlp_join.hpp), gives the 16 bytes of
// the jump to ns:s as the system's Download Play application does (dlp_join_sys.hpp) and chain-loads the hidden prepared child (main.cpp): the child's
// GetWirelessRebootInfo is answered by the system, never by the plugin.
//   - JoinGate: what stops the page before anything starts (the installed app, Luma3DS and its Plugin Loader, the plugin on, the room the child joins
//     by itself - clnt::UsesRoom, the plugin's own formula -, the HOME Menu for the jump, no work under way, children ready).
//   - BuildTable: the table of the children AS THE PLUGIN OF THE CHILD WILL SEE IT. It is built from the index FILE the plugin reads (its text cut
//     where the plugin cuts it), and a row is Ready only when an Installed record of the app says the same child, game, prepared title and version,
//     the title is ours (dlpguest::IsOwnedGuestTid), AM says it is there at that version and the plugin is in its folder. An Installed record with no
//     row in the file is "index not written": the child would start with IsChild false after the app has left the host.
//   - ShowOf, PhaseOf, FriendSeedOf, ComposeRebootInfo: what a row shows, what the session says, the lobby's seed (the plugin's), the 16 bytes.
//   - ScanLine, ScanLog (the build 2f, B3): one line of the log per scan that changed, kept in memory and written when the session ends.
// Pure header: no file, no log, no kernel, no lock. Tests/dlp_join_test.cpp (J8, J9, J11) holds it.
#pragma once

#include <cstddef>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "azl/common.hpp"
#include "azl/dlp_children.hpp"  // (the table, the verdicts, the index and its reader: the plugin's own)
#include "azl/dlp_clnt_map.hpp"  // (clnt::UsesRoom: does the child join the room by itself)
#include "azl/dlp_lobby.hpp"     // (Group, the client states)
#include "azl/room_client.hpp"   // (RoomClient::State)
#include "azl/title_rules.hpp"
#include "dlp_guest_store.hpp"   // (the app's records of its prepared children, Presence, IsOwnedGuestTid)

namespace dlpjoin {

using azl::s32;
using azl::u16;
using azl::u32;
using azl::u64;
using azl::u8;

// ---- what stops the page -----------------------------------------------------------------------------------------------------------------
/// The first reason the page does not start, in this order (Ok: nothing stops it). NoChildPlugin is not one: it is a verdict of a host (ShowOf).
enum class Why : u8 {
    Ok,
    NotInstalled,   ///< a .3dsx (the Homebrew Launcher's way out is not the HOME Menu's: the jump is not proven there)
    LumaTooOld,     ///< Luma3DS older than 13.0: no plugin in any title
    LoaderOff,      ///< Rosalina's Plugin Loader is off: the child would start without the plugin
    PluginOff,      ///< 3DS Link off in the settings (enabled && tunnel): the child's IsChild says false
    NoRoom,         ///< no room in the settings: the child would join none
    NoAutoConnect,  ///< the child does not join the room by itself (clnt::UsesRoom)
    NoHomeMenu,     ///< no HOME Menu to make the jump (an emulator that was given the title)
    WorkRunning,    ///< a job the way out of main.cpp waits for (AnyWork)
    GuestWorker,    ///< the worker of the Download Play children is running (dlpguest::Running)
    DeliveryUp,     ///< a room of Titles Delivery is up
    NoGuests,       ///< no child Ready in the table
};
/// The jobs the way out of main.cpp waits for: with none of them the exit after the passphrase is short (the host waits kLeaveMs = 5 s at most).
struct Work {
    bool copies;        ///< copies of the plugin waiting (setup::CopyWorker)
    bool titles_scan;   ///< the scan of the titles
    bool update;        ///< the update's worker
    bool lobbies_scan;  ///< the asking of the public rooms (it goes on a moment after its page is left)
};
inline bool AnyWork(const Work& w) {
    return w.copies || w.titles_scan || w.update || w.lobbies_scan;
}
/// (the review of the build 2h, U3: "Join does not wait for work that is not its own") what the page waits for before its session starts: the update
/// and the public lobbies (the network). The copies of the plugin and the scan of the titles only at A, before the way out (spec 8 3.2: the exit
/// after the passphrase must be short) - never at the entrance.
inline bool EntryWork(const Work& w) {
    return w.update || w.lobbies_scan;
}
struct Facts {
    bool installed_app;  ///< !envIsHomebrew()
    bool luma_ok;        ///< Luma3DS 13.0 or later
    bool loader_on;      ///< Rosalina's Plugin Loader on
    bool plugin_on;      ///< cfg.enabled && cfg.tunnel: the plugin of the child is active (plugin.cpp: active_), so IsChild says true
    bool room_set;       ///< a room in the settings (host not empty)
    bool autoconnect;    ///< cfg.autoconnect
    bool menu_off;       ///< the in-game menu is off (setup::MenuOn false)
    bool home_menu;      ///< APT_IsRegistered(the HOME Menu): the chain-load's way
    bool work_running;   ///< AnyWork
    bool guest_worker;   ///< dlpguest::Running()
    bool delivery_up;    ///< a room of Titles Delivery is up
    u32 guests_ready;    ///< rows Ready in the table (ReadyCount)
};
inline Why JoinGate(const Facts& f) {
    if (!f.installed_app) {
        return Why::NotInstalled;
    }
    if (!f.luma_ok) {
        return Why::LumaTooOld;
    }
    if (!f.loader_on) {
        return Why::LoaderOff;
    }
    if (!f.plugin_on) {
        return Why::PluginOff;
    }
    if (!f.room_set) {
        return Why::NoRoom;
    }
    if (!azl::clnt::UsesRoom(f.room_set, f.autoconnect, f.menu_off)) {
        return Why::NoAutoConnect;
    }
    if (!f.home_menu) {
        return Why::NoHomeMenu;
    }
    if (f.work_running) {
        return Why::WorkRunning;
    }
    if (f.guest_worker) {
        return Why::GuestWorker;
    }
    if (f.delivery_up) {
        return Why::DeliveryUp;
    }
    if (f.guests_ready == 0) {
        return Why::NoGuests;
    }
    return Why::Ok;
}
/// The page's sentence for a refusal (English, one line of the card).
inline const char* WhyText(Why w) {
    switch (w) {
    case Why::Ok: return "";
    case Why::NotInstalled: return "Start 3DS Link from the HOME Menu (the installed app) to join";
    case Why::LumaTooOld: return "Luma3DS 13.0 or later is needed: see the Installation check";
    case Why::LoaderOff: return "Turn Rosalina's Plugin Loader on: see the Installation check";
    case Why::PluginOff: return "Turn 3DS Link on in Settings";
    case Why::NoRoom: return "Choose an Azahar Emulator Room first";
    case Why::NoAutoConnect: return "Turn on Auto-connect in Settings: the game must join the room by itself";
    case Why::NoHomeMenu: return "The HOME Menu is not there to start the game";
    case Why::WorkRunning: return "Wait for the work under way to end (copies, scan, update, public lobbies)";
    case Why::GuestWorker: return "3DS Link is working on the Download Play guests: wait a moment";
    case Why::DeliveryUp: return "Close Titles Delivery first";
    case Why::NoGuests: return "No game is ready for Download Play: enable one in Download Play > Enable games";
    }
    return "";
}

// ---- a host of the list ----------------------------------------------------------------------------------------------------------------------
struct Host {
    azl::Group g;                     ///< the lobby's entry (name, child, total/max)
    azl::dlp_children::Verdict v;     ///< its child on this console (the table, the sizes its frame 0 announced)
    u64 prepared;                     ///< the row's prepared title (its 'prepared' field), 0 = none
    bool full;                        ///< total >= max: nobody else gets in
    bool index_row;                   ///< false: an Installed child of ours that the index file does not have ("index not written")
    const u8* block;                  ///< the child's block (names, icon), or null
};
/// What a row shows: Join (A enters it) only for a Ready child, in the index file, a host that is not full; Hidden for a host whose child has no row
/// (another game, counted at the bottom); Grey with its reason for the rest.
enum class Show : u8 { Join, Grey, Hidden };
inline Show ShowOf(const Host& h) {
    if (!h.index_row) {
        return Show::Grey;
    }
    if (h.v == azl::dlp_children::Verdict::NoRow) {
        return Show::Hidden;
    }
    if (h.v == azl::dlp_children::Verdict::Ready && !h.full) {
        return Show::Join;
    }
    return Show::Grey;
}

/// A member of the room hosts one of the lobbies the scan sees: its room MAC is a lobby's host (a console in a room - Azahar, or a game with 3DS
/// Link - announces its lobby with the room's MAC). Not by name: the beacon carries the console's profile name (at most 10 units), the room a
/// nickname of its own, and the two are often different.
inline bool HostsALobby(const std::vector<Host>& hosts, const azl::Mac& member) {
    for (const Host& h : hosts) {
        if (h.g.host == member) {
            return true;
        }
    }
    return false;
}

// ---- the session -----------------------------------------------------------------------------------------------------------------------------
enum class Phase : u8 {
    Off,
    Joining,     ///< the room is being joined
    Scanning,    ///< the list
    Entering,    ///< a join under way (the lobby's word says 1 then: the Core's flag tells it)
    Waiting,     ///< 5: in the host's lobby
    Starting,    ///< 6: the host started
    Passphrase,  ///< 9: the passphrase came
    Handing,     ///< the 16 bytes to ns:s, the room left (the page's)
    Jumping,     ///< the app ends for the child (the page's)
    HostLeft,    ///< 0x80: the host is gone
    RoomLost,    ///< the room's link is down
    Failed,      ///< ns:s refused, or the room refused us (the page's)
};
inline Phase PhaseOf(u32 lobby_word, bool joining, azl::RoomClient::State room) {
    if (room != azl::RoomClient::State::Joined) {
        return room == azl::RoomClient::State::Idle ? Phase::RoomLost : Phase::Joining;
    }
    if (joining) {
        return Phase::Entering;
    }
    switch (lobby_word >> 24) {
    case azl::kCliOff: return Phase::Off;
    case azl::kCliIdle:
    case azl::kCliScanning: return Phase::Scanning;
    case azl::kCliInSession: return Phase::Waiting;
    case azl::kCliStarted: return Phase::Starting;
    case azl::kCliComplete: return Phase::Passphrase;
    case azl::kCliLost: return Phase::HostLeft;
    default: break;
    }
    return Phase::Scanning;
}

/// The lobby's friend code seed: FNV-1a 64 of the console's MAC, as the plugin's MakeClntHub makes it (Fnv1a64 in plugin.cpp).
inline u64 FriendSeedOf(const azl::Mac& mac) {
    u64 hash = 0xCBF29CE484222325ull;
    for (const u8 b : mac) {
        hash ^= b;
        hash *= 0x100000001B3ull;
    }
    return hash;
}

/// The 16 bytes the system's Download Play application gives ns:s before its jump (modalita-download/RAPPORTO.md 4.3): [0..5] the host's MAC,
/// [6..14] the passphrase, [15] 0. Never logged, never written to a file.
inline void ComposeRebootInfo(const u8 (&mac)[6], const u8 (&passphrase)[9], u8 (&out)[16]) {
    std::memcpy(out, mac, 6);
    std::memcpy(out + 6, passphrase, 9);
    out[15] = 0;
}

/// Bytes the compiler may not leave out (a key, a passphrase, a MAC): every byte written through a volatile pointer.
inline void Wipe(void* p, std::size_t n) {
    volatile u8* v = static_cast<volatile u8*>(p);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = 0;
    }
}

// ---- the table of the children, as the plugin of the child will see it ----------------------------------------------------------------------
/// The index text as Plugin::ReadDataFile gives it to the plugin: at most kIndexMaxBytes, and a longer file cut after its last whole line within them.
inline std::string IndexAsPluginReads(const std::string& whole) {
    if (whole.size() <= azl::dlp_children::kIndexMaxBytes) {
        return whole;
    }
    std::string out = whole.substr(0, azl::dlp_children::kIndexMaxBytes);
    const std::size_t last = out.rfind('\n');
    out.resize(last == std::string::npos ? 0 : last + 1);
    return out;
}

/// The table and the blocks it points to (16 x 5,000 B: on the heap, never on a stack).
struct Children {
    azl::dlp_children::Table table;
    u8 blocks[azl::dlp_children::kMaxRows][azl::dlp_children::kBlockBytes];
};
using PresenceFn = std::function<dlpguest::Presence(u64 prepared, u16* version)>;    ///< AM (dlpguest::PreparedPresence)
using FileFn = std::function<bool(u64 prepared)>;                                     ///< the plugin in the prepared title's folder
using BlockFn = std::function<bool(u64 prepared, u8* out)>;                            ///< the block, kBlockBytes whole

/// The table of the children from `index_text` (`size` bytes as the plugin reads them, nullptr = no file) and the app's records `guests`. A row is
/// Ready only when an Installed record of ours (IsOwnedGuestTid) says the same child, game, prepared title and version, AM says the prepared title is
/// Present at that version, and its plugin is there; a row with no such record says NotPrepared (never Ready: the file is what the plugin believes,
/// the record is what the app made). Every row's block is read now (PutBlock or NoBlock: none is left "not read"). `missing_row` gets the CHILD of
/// every Installed record of ours whose prepared title has no row in the file ("index not written": the plugin of that child would not know it).
/// True when the file was an index (its header).
inline bool BuildTable(const char* index_text, std::size_t size, const std::vector<dlpguest::Guest>& guests, const PresenceFn& present,
                       const FileFn& plugin_there, const BlockFn& block, Children* out, std::vector<u64>* missing_row) {
    namespace dc = azl::dlp_children;
    dc::Table& t = out->table;
    dc::Clear(&t);
    missing_row->clear();
    t.index_found = index_text != nullptr && dc::ParseIndex(index_text, size, &t.index);
    if (!t.index_found) {
        std::memset(&t.index, 0, sizeof(t.index));
    }
    struct Ctx {
        const dc::Index* index;
        const dlpguest::Guest* match[dc::kMaxRows];
        const PresenceFn* present;
        const FileFn* plugin;
    } ctx = {&t.index, {}, &present, &plugin_there};
    for (std::size_t r = 0; r < t.index.count; ++r) {
        const dc::Row& row = t.index.rows[r];
        for (const dlpguest::Guest& g : guests) {
            if (g.state == dlpguest::GuestState::Installed && row.prepared != 0 && g.prepared == row.prepared && g.child == row.child &&
                g.game == row.game && g.child_version == row.version && dlpguest::IsOwnedGuestTid(row.prepared)) {
                ctx.match[r] = &g;
                break;
            }
        }
    }
    dc::Probe probe;
    probe.ctx = &ctx;
    probe.title_present = [](void* c, u64 title_id, u8 media, u32 content) -> bool {
        const Ctx& x = *static_cast<const Ctx*>(c);
        if (content != 0) {
            return true;  // (the game: its record was made from it, and the worker takes the child away when the game goes)
        }
        const int r = dc::FindPrepared(*x.index, title_id);
        u16 version = 0;
        return r >= 0 && x.match[r] != nullptr && (*x.present)(title_id, &version) == dlpguest::Presence::Present && version == x.index->rows[r].version;
    };
    probe.plugin_present = [](void* c, u64 prepared) -> bool {
        const Ctx& x = *static_cast<const Ctx*>(c);
        const int r = dc::FindPrepared(*x.index, prepared);
        return r >= 0 && x.match[r] != nullptr && (*x.plugin)(prepared);
    };
    dc::JudgeAll(t.index, t.verdict, probe, dc::PresenceBy::Am);
    for (std::size_t r = 0; r < t.index.count; ++r) {
        const u64 prepared = t.index.rows[r].prepared;
        if (prepared != 0 && block(prepared, out->blocks[r])) {
            dc::PutBlock(&t, r, out->blocks[r]);
        } else {
            dc::NoBlock(&t, r);
        }
    }
    for (const dlpguest::Guest& g : guests) {
        if (g.state == dlpguest::GuestState::Installed && dlpguest::IsOwnedGuestTid(g.prepared) && dc::FindPrepared(t.index, g.prepared) < 0) {
            missing_row->push_back(g.child);
        }
    }
    return t.index_found;
}

/// The rows a host can be joined for: Ready with their block read.
inline u32 ReadyCount(const azl::dlp_children::Table& t) {
    u32 n = 0;
    for (std::size_t r = 0; r < t.index.count && r < azl::dlp_children::kMaxRows; ++r) {
        n += t.verdict[r] == static_cast<u8>(azl::dlp_children::Verdict::Ready) &&
                     azl::dlp_children::BlockStateOf(&t, r) == azl::dlp_children::BlockState::Read
                 ? 1u
                 : 0u;
    }
    return n;
}

// ---- the log of the scans (the build 2f, B3 of analysis/piano-1.1.4-2026-11-15/prova-2e-2026-11-19/ANALISI.md 4) ---------------------------------
/// What a host's row is, in a word of the log (the page's GreyValue, and "no row" for a host of another game, which the page only counts).
inline const char* ScanState(const Host& h) {
    using V = azl::dlp_children::Verdict;
    if (!h.index_row) {
        return "grey index not written";
    }
    if (h.v == V::Ready) {
        return h.full ? "full" : "ready";
    }
    switch (h.v) {
    case V::NoRow: return "no row";
    case V::OtherVersion: return "grey another version";
    case V::NotPrepared: return "grey not prepared";
    case V::NoChildPlugin: return "grey no plugin";
    case V::NoGame: return "grey no game";
    default: return "grey not known yet";
    }
}
/// The mode of the Download Play column of a game's row (azl/title_rules.hpp DlpOf), in a word of the log.
inline const char* ModeWord(azl::DlpGameMode m) {
    switch (m) {
    case azl::DlpGameMode::ProbeOnly: return "ProbeOnly";
    case azl::DlpGameMode::Lobby: return "Lobby";
    case azl::DlpGameMode::RealDlp: return "RealDlp";
    case azl::DlpGameMode::SystemClient: return "SystemClient";
    default: return "none";
    }
}
/// One line of the log for the hosts the page sees now: "dlp: scan: N host(s): <child low> <game id or "another game"> <mode> <state> <total>/<max>;
/// ...; members waiting M". The game is the row of the table whose child the host announces (its id: the index's, never a name). Never a MAC, a
/// nickname, a host's name or a passphrase (dlp_join.hpp: the log has states and times only).
inline std::string ScanLine(const std::vector<Host>& all, const Children& t, std::size_t members_waiting) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    const auto hex8 = [](u32 v) {
        std::string s(8, '0');
        for (int i = 7; i >= 0; --i) {
            s[static_cast<std::size_t>(i)] = kDigits[v & 0xF];
            v >>= 4;
        }
        return s;
    };
    std::string line = "dlp: scan: " + std::to_string(all.size()) + (all.size() == 1 ? " host" : " hosts");
    const char* sep = ": ";
    for (const Host& h : all) {
        u64 game = 0;
        for (std::size_t r = 0; r < t.table.index.count && r < azl::dlp_children::kMaxRows; ++r) {
            if (static_cast<u32>(t.table.index.rows[r].child) == h.g.child_tid_low) {
                game = t.table.index.rows[r].game;
            }
        }
        line += sep;
        sep = "; ";
        line += hex8(h.g.child_tid_low) + " ";
        if (game != 0) {
            char id[17];
            azl::dlp_children::PutHex16(id, game);
            id[16] = '\0';
            line += std::string("game ") + id + " " + ModeWord(azl::title_rules::DlpOf(game).mode);
        } else {
            line += "another game";
        }
        line += std::string(" ") + ScanState(h) + " " + std::to_string(h.g.total_nodes) + "/" + std::to_string(h.g.max_nodes);
    }
    return line + "; members waiting " + std::to_string(members_waiting);
}
/// The lines of the scans of a session, kept in memory (the page notes one per frame; only a line that differs from the last is kept) and written
/// once, when the session ends (dlp_join.cpp Session::Stop, in the same write as its own lines): never a write of the SD card while the page looks,
/// never one on the room's thread. The LAST kLines lines are kept - the ones nearest the jump or the exit -; the earlier ones are counted. Each line
/// carries "(+N ms)" from the first scan of the session: the log stamps the whole batch with the time of the write. The price, said: a crash loses
/// them. (The review of the build 2f: the first 32 were kept, with no time of their own)
struct ScanLog {
    static constexpr std::size_t kLines = 32;
    std::vector<std::string> lines;
    std::size_t dropped = 0;
    std::string last;
    u64 first_ms = 0;
    bool any = false;
    void Note(const std::string& line, u64 now_ms) {
        if (line == last) {
            return;
        }
        last = line;
        if (!any) {
            any = true;
            first_ms = now_ms;
        }
        if (lines.size() >= kLines) {
            lines.erase(lines.begin());
            ++dropped;
        }
        lines.push_back(line + " (+" + std::to_string(now_ms - first_ms) + " ms)");
    }
    /// The lines to write ("dlp: scan: N earlier lines dropped" before them when there were more), and the log empty again.
    std::vector<std::string> Take() {
        std::vector<std::string> out;
        if (dropped > 0) {
            out.push_back("dlp: scan: " + std::to_string(dropped) + " earlier lines dropped (kept the last " + std::to_string(kLines) + ")");
        }
        out.insert(out.end(), lines.begin(), lines.end());
        lines.clear();
        dropped = 0;
        last.clear();
        any = false;
        first_ms = 0;
        return out;
    }
};

}  // namespace dlpjoin
