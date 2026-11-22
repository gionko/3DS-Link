// The rules of 3DS Link that hold for some games only: one row per game, and nothing else to change (1.1.4, 2026-11-15).
//
// Every game runs with the defaults (kNoRules): every frame it sends goes out, one packet of the room reaches it per round of the
// network thread, a command of the room that finds ENet's limit of waiting data is refused (and sent again by the room), a host
// answers every newcomer, and the Download Play services are the real ones. A row turns on, for the game it names, only the rules a capture
// or a session of THAT game showed it needs; the plugin reads the row in one place (Plugin::ApplyConfig, Sources/ctr/plugin.cpp, and
// Plugin::OnRoomInfo for the host's rule; the Download Play column in Plugin::EarlyInit, through DlpModeOf and its neighbours).
//
// To add a game: one row, with the ids of ALL its regions (the catalogue of the app's lobbies: the low word of the application's title
// id; its Download Play child, 00040001, is the same row) and the rules it needs. Tests/flow_relief_test.cpp holds every row to the whole
// family of its game in that catalogue (Tests/title_families.inc): an id left out is one of kLeftOut, with its reason. The compiler holds every row to the bounds below
// (RowsAreSound), and the tests run every row (Tests/flow_relief_test.cpp, Tests/test_main.cpp): a new row needs no new test, no new
// pin of Tests/policy_check.sh and no new mutant. A bound itself changes only with a measurement that says why. The Download Play column
// (below) is the one exception: Tests/plugin_dlp_test.cpp writes every id of it by hand (AZL_DLP_AS_BEFORE, its table, its count, and
// Tests/policy_check.sh holds the column to those lines), so an id added, moved or changed there changes those lines in the same commit; and a
// game that is given the REAL Download Play (DlpGameMode::RealDlp) comes with its own Azahar - Azahar capture and its own test (design F7).
//
// The rules (each one's reason is where its code is):
//   - same_frame_ms (azl/flow_relief.hpp, SameFrame): a broadcast data frame that is byte for byte the last one sent on its channel,
//     less than this many ms ago, is not sent again; the game still gets "success". Only for a game whose broadcast frames are STATES
//     (each one carries all that is not confirmed yet), never for one whose frames are events. 0 = off.
//   - hand_over_burst (RoomClient::SetHandOverBurst): up to this many of the room's packets that are ready reach the game in one round,
//     instead of one. 1 = off.
//   - cap_drop_max (RoomClient::SetCapDrop, ENetHost::azlCapDrop): at ENet's limit a relayed game frame or beacon (what a radio may lose)
//     is acknowledged and dropped instead of refused, with a placeholder that keeps its sequence number; at most this many placeholders
//     wait at once (about 100 bytes of the plugin's heap each), beyond that refused as before. The room's own messages are never
//     dropped. 0 = off.
//   - host_keeps_to_its_game (azl/one_game_host.hpp): a console that hosts the game as the APPLICATION answers no newcomer the room
//     lists with another sold game. false = off.
//   - send_budget_bytes (RoomClient::SetSendBudget, 2026-11-15): a round of the network thread hands ENet at most this many bytes of the
//     game's queue (the first frame always), and the rest waits for the next round, which reads the socket first: a burst of the game's
//     frames no longer keeps the thread in its sends for half a second while the socket's ring fills. 0 = off (everything, as ever).
// The start-up rules (2026-11-15: they were three lists of their own, azl/core_plan.hpp and azl/soc_plan.hpp; read by the APPLICATION's id only,
// AppRulesFor - a Download Play child starts as it ever did):
//   - worker_off_core1 (coreplan::LeavesCore1ToTheGame, ctr/thread_core.cpp): on a New 3DS too, the framework's start-up threads leave core 1, where
//     the game's own thread is refused (0xE0A01839) while the framework's worker holds it (azl/core_plan.hpp says why). false = off.
//   - late_soc_block (socplan::TakesBlockLate, ctr/soc.cpp): the soc block is taken at the first connection, not before the game sizes its heap
//     (azl/soc_plan.hpp says why). false = off.
//   - soc_guest (socplan::SharesGameSockets, ctr/soc.cpp): a game that opens the system's sockets itself first; the plugin becomes a guest in its
//     context instead of giving up. false = off.
// The Download Play column (Dlp, 1.1.4: it was kDlpGames of Sources/ctr/plugin.cpp; read by the APPLICATION's id only, DlpOf - a Download Play
// child, an update or a system title has no column, as the plugin's table answered for whole ids): what the plugin does with the Download Play
// services (dlp:FKCL, dlp:SRVR) of the game. kNoDlp (mode None) for every game that does not say it: the real services, as before.
//   - ProbeOnly: the games whose wireless mode (UDS, every player with their own copy) starts by asking dlp:FKCL whether the wireless is on and
//     then never uses Download Play. dlp:FKCL is answered by the plugin (azl/dlp_fkcl_hle.hpp); dlp:SRVR stays the real one. Another game may
//     need the same as Battle Royale: check in its log that the only dlp:FKCL commands are Initialize / InitializeWithName and Finalize
//     (anything else is refused and written as "dlp: dlp:FKCL command 0x.. refused").
//   - Lobby: the games whose wireless mode goes on with the Download Play proper. The room carries their lobby: BOTH services are served by
//     the plugin's DlpHub (azl/dlp_hub.hpp), the host's announcement and the four messages of the creator go over the room like the game's
//     own UDS frames, in the plugin's own dialect, and the engine is given back to the game's nwm::UDS whole when the game starts it.
//   - RealDlp (1.1.3): the same hub and the same lobby, on the adapter of the REAL Download Play (azl/dlp_real_net.hpp), which speaks on the
//     wire what Azahar's Download Play emulates (the "fake client" case: every player has the game, nothing is downloaded), so that a console
//     and Azahar see each other's lobby. Its beacon and its messages are not the dialect's: a console with the game in Lobby and one with it
//     in RealDlp do not see each other in this game. Moving a game between Lobby and RealDlp is its row, and nothing else: no switch at run
//     time (design PROGETTO-DLP-VERO-1.1.3.md 3.4). Only the games in Lobby and RealDlp get a hub, and only the RealDlp ones its adapter.
//   - console_seed (Lobby / RealDlp): the lobby announces the console's own friend code seed (config block 0x00090000, what the game itself
//     gives nwm::UDS, and what the real Download Play modules give) instead of the hash of the MAC. Mario Tennis Open keeps the players of the
//     lobby in a table and, when its own network starts, gives each one back its id by comparing the seed of the lobby with the seed of the
//     player's nwm::UDS node (read with the debugger from a stalled game, 2026-11-01): with the hash of the MAC the two never match and the
//     game's synchronisation never finishes. The other Lobby games (Kirby) do not compare them. false = the hash of the MAC.
//   - max_players (2026-11-13, "a game that plays in eight is played in eight"): the ceiling of the lobby of this game, host included: four
//     (kDlpTitlePlayers) unless the row says more - on the wire and in every answer what those games had when the lobby's tables held four
//     players (Tests/dlp_cap_guard_test.cpp holds it) - and only a RealDlp row says more (Mario Kart 7, whose creator asks for seven clients).
//     It goes to the lobby in LobbyConfig (LobbyPlayersOf, MakeDlpHub / MakeRealDlpHub) and to the adapter of the real Download Play (its SetPlayers).
// The announcement of the child's title, names and icon is every application's whose host has the real Download Play (dlp_real::AnnouncesTitle,
// azl/dlp_title_block.hpp): no column.
// The PREPARED CHILD of a game (1.1.4, K1 of analysis/piano-1.1.4-2026-11-15/specifiche/7-CONTRATTO.md): the title the app installs, hidden,
// so that the system's Download Play application can start the game's child without downloading it. One formula, on the GAME, for every
// game: PreparedOf. Which child a prepared title is, is the index's (azl/dlp_children.hpp), never a formula read backwards: IsPreparedId
// says the band only. No column: a row says nothing of it.
// Pure header: no engine, no room, no allocation; every function is a constant expression.
#pragma once

#include <cstddef>

#include "azl/common.hpp"
#include "azl/dlp_mode.hpp"

namespace azl {
namespace title_rules {

inline constexpr u32 kTitleHighApplication = 0x00040000u;
inline constexpr u32 kTitleHighDlpChild = 0x00040001u;
/// (the build 2h, 2026-11-19) the category of the system's applications: the ONE id of a prepared Download Play child (ticket, TMD, NCCH, exheader,
/// the jump). HOME does not show one of the SD card (prova-2e-2026-11-19/prova-pc/PROVA-PC.md M3a/M3avis), Luma3DS gives it the plugin
/// ((high & 0xFFFFFFED) == 0x00040000, loader.c:456) and pm, NS and AM know it by the same id (one id: pm/source/launch.c:56, manager.c:71).
inline constexpr u32 kTitleHighSystemApplication = 0x00040010u;

/// The bounds every row is held to.
/// A frame equal to the last one is held back for less than a step of a game at 30 steps a second: the other side still hears every step.
inline constexpr u32 kSameFrameMsMax = 33;
/// A round hands over at most one read pass of ENet (ENET_AZL_READ_MAX_DATAGRAMS, 32: Tests/flow_relief_test.cpp holds the two equal).
inline constexpr u32 kHandOverBurstMax = 32;
/// About 100 bytes of the plugin's heap a placeholder: 2048 are about 200 KB, held only while the limit is reached (2026-11-14: 1286 KB
/// of the heap were free at the lowest on a New 3DS XL with Island Tour, in the crisis the placeholders are for; the heap is the
/// plugin's own, as large on an Old 3DS).
inline constexpr u32 kCapDropMaxBound = 2048;
/// A budget is at least one datagram of the room's MTU (1392 bytes: a frame of 1476 bytes still goes in one round with what fits beside it) and
/// at most 64 KB (more is a round of 45 sends of 4-5 ms, the very stall it is for).
inline constexpr u32 kSendBudgetMin = 1392;
inline constexpr u32 kSendBudgetMax = 65536;
/// The ids a row can hold (the regions of one game, its re-releases in the catalogue too: Ocarina of Time 3D has nine).
inline constexpr std::size_t kMaxRegions = 10;
/// The ceilings of a Download Play column: four players unless the row says more, eight at the most - the lobby's kTitlePlayers and
/// kMaxPlayers, written here so that the table of the titles does not include the lobby's types (Sources/ctr/plugin.cpp holds the two pairs
/// equal, at compile time).
inline constexpr u8 kDlpTitlePlayers = 4;
inline constexpr u8 kDlpPlayersMax = 8;

struct Rules {
    u32 same_frame_ms;           ///< 0 = every frame is sent
    u32 hand_over_burst;         ///< 1 = one packet of the room a round
    u32 cap_drop_max;            ///< 0 = refused at ENet's limit
    bool host_keeps_to_its_game; ///< false = a host answers every newcomer
    u32 send_budget_bytes;       ///< 0 = a round sends all the game's queue
    bool worker_off_core1;       ///< false = the framework's start-up threads stay where they are on a New 3DS
    bool late_soc_block;         ///< false = the soc block is taken at start-up
    bool soc_guest;              ///< false = the plugin opens its own sockets (and gives up if the game was first)
};

/// What every game that has no row runs with.
inline constexpr Rules kNoRules = {0, 1, 0, false, 0, false, false, false};

/// The Download Play column of a row (the header says what each value does).
struct Dlp {
    DlpGameMode mode;   ///< None = the real dlp:FKCL / dlp:SRVR
    bool console_seed;  ///< the lobby announces the console's own friend code seed (cfg 0x00090000), not the hash of the MAC
    u8 max_players;     ///< the ceiling of the game's lobby, host included: kDlpTitlePlayers unless the row says more (RealDlp only)
};
/// What every game that has no Download Play column runs with.
inline constexpr Dlp kNoDlp = {DlpGameMode::None, false, kDlpTitlePlayers};

struct Row {
    const char* name;        ///< for the log
    u32 low[kMaxRegions];    ///< the low words of the application's title ids, one per region; the unused ones are 0, at the end
    Rules rules;
    Dlp dlp = kNoDlp;  ///< the Download Play column (kNoDlp: none)
};

inline constexpr Row kRows[] = {
    // Mario Party: Island Tour (family ATS): Japan, USA, Europe, Taiwan, Korea. Only the European application has been seen on a
    // console. Its broadcast frames are states, 180 a second from each side at the dice (2026-11-14, azl/flow_relief.hpp); at the
    // start of a minigame a console's network thread stays in its sends up to 0.5 s and the room's stream fills the limit behind a lost
    // datagram (2026-11-14 22:50: the room ended the link after 14 minutes; with the drop and 2048 placeholders the model has no
    // failure in 24 runs, analysis/prova-1.1.4-2026-11-14/friend_a-prova-quattro/causa/VERDETTO.md); a member of the room with
    // Tetris open held a place of its network (2026-11-14 evening). A console that hosts it spent 400-540 ms in its sends in a crisis
    // (2026-11-15, 22 minutes: 207 copies sent again by the room, 178 numbers lost while the thread was away): 4 KB of the game's queue a round.
    // Download Play (2026-11-14: the console HOSTS): RealDlp, the hash of the MAC, four players. Read from the European game's code: its "Party" is the
    // Download Play and nothing else, and the game has only the host's side - dlp:SRVR IsChild at boot, Initialize (0x01, no name) for 3 clients
    // with the child 0, StartAccepting by hand, then AcceptClient for every client it reads in 0x40 and StartDistribution in the same frame, the
    // passphrase once, and its own network (wlancommid 000F8210, four nodes), where it waits 20 s for every player it counted. With no column the
    // real module answers 0xC940AC04 and the game says that the wireless is off. Who joins is the system's Download Play application, which then
    // starts the child 00040001000F8200: an Azahar that has the child installed. The contract is the capture of two Azahar that played it so
    // (2026-11-13; Tests/dlp_real_island_golden.inc, and Tests/dlp_real_island_test.cpp holds the hub to it as the game drives it, with three
    // clients; in the capture the seeds of the Download Play's network are zero and the game played). What this column does NOT do: a guest that
    // has no child asks for its content and is ejected (the child never travels through the room) while the game still counts it and waits for
    // it; a console cannot JOIN (the plugin is loaded neither in the system's Download Play nor in the child). Two limits read from the game's
    // code, held as they are by part L of that test: the game sends StartDistribution and the passphrase ONCE and drops their result, so if the
    // hub refuses one (no Tick of the network thread for a second; HOME or an applet in that instant) the lobby stays where it is until the player
    // leaves it and comes back; and the game's "back" while it distributes (EndAccepting and StartAccepting in state 6) is answered and not
    // followed. Never seen on a console in this mode. The other regions (2026-11-14, every region of a game) have the values of the European game;
    // their code was NOT read and no Azahar played them. Nothing the plugin does for this game is the European id's: the child the host announces
    // is 00040001 above the low word of the game's OWN title with the index the game gives (Tests/dlp_real_island_test.cpp, part R, holds it for
    // the five ids). Whether a regional build drives dlp:SRVR as the European one does is not known.
    {"Mario Party: Island Tour", {0x000DD400u, 0x000F8100u, 0x000F8200u, 0x00119300u, 0x00119B00u}, {30, 16, 2048, true, 4096, false, false, false},
     {DlpGameMode::RealDlp, false, 4}},
    // The early 2011 games whose NintendoWare sound thread asks for core 1 a moment after the start (azl/core_plan.hpp): the same ErrDisp at the same
    // address every time on a New 3DS, and the game plays with the title off in Enable Titles.
    // Ridge Racer 3D (family ARR): Japan, Europe, USA. The European one seen (ErrDisp 0x352FB0, 2026-11-10; with the row it plays on the 63, 29.8 fps).
    {"Ridge Racer 3D", {0x00032800u, 0x00033B00u, 0x00035800u}, {0, 1, 0, false, 0, true, false, false}},
    // Pac-Man & Galaga Dimensions (family APG): USA (ErrDisp 0x419A94, 2026-11-10), Japan, Europe (ErrDisp 0x123778, Friend-A 2026-10-21).
    {"Pac-Man & Galaga Dimensions", {0x00036300u, 0x00039900u, 0x00045D00u}, {0, 1, 0, false, 0, true, false, false}},
    // The Legend of Zelda: Ocarina of Time 3D (family AQE): Japan, USA, Europe (ErrDisp 0x4660F4, Friend-A 2026-10-21) and the six other ids of the
    // catalogue (Europe 57300, USA 57400 / 5B800 / 5FC00, Korea 8F800, Taiwan 8F900: re-releases or demos, never seen; moving a worker off core 1 is
    // what an Old 3DS does for every title).
    {"The Legend of Zelda: Ocarina of Time 3D", {0x00033400u, 0x00033500u, 0x00033600u, 0x00057300u, 0x00057400u, 0x0005B800u, 0x0005FC00u, 0x0008F800u, 0x0008F900u},
     {0, 1, 0, false, 0, true, false, false}},
    // Monster Hunter XX (family AGQ): Japan (its heap sized to the byte by the commit: the soc block at the first connection, 2026-11-01) and Taiwan
    // (never seen: the same game).
    {"Monster Hunter XX", {0x00197100u, 0x001B8100u}, {0, 1, 0, false, 0, false, true, false}},
    // Fantasy Life (family AFL): Europe (it opens soc:U itself at 1.9 s, Friend-A 2026-11-08), USA, Japan.
    {"Fantasy Life", {0x00113100u, 0x00113200u, 0x0006D200u}, {0, 1, 0, false, 0, false, false, true}},
    // ---- The games with a Download Play column only (1.1.4: they were kDlpGames of Sources/ctr/plugin.cpp, every value as it was). EVERY REGION OF A
    // GAME (2026-11-14): the European game is the one that is tried, and an American, a Japanese, a Korean or a Taiwanese copy must not be left with
    // the real services ("wireless off") for that; the ids that say "catalogue" are 3DS Link's catalogue of sold games with the values of the game
    // that WAS seen: their code was not read and nobody played them with the plugin. Left out on purpose (kLeftOut): the kiosk demos that share a
    // family with the game and an id nobody identified.
    // Kirby Battle Royale (family AJ8): 1C2100 seen (InitializeWithName, Finalize 1 ms later, then nwm::UDS only); 1C2000 another region's id, not
    // seen; Japan 1A8B00 and Korea 1C2200 from the catalogue.
    {"Kirby Battle Royale", {0x001C2100u, 0x001C2000u, 0x001A8B00u, 0x001C2200u}, kNoRules, {DlpGameMode::ProbeOnly, false, 4}},
    // Kirby Triple Deluxe (family BAL): seen on a console, 10C000 (dlp:FKCL InitializeWithName, GetChannels, then the scan; with the probe of
    // 2026-10-28 the game answered the refusal with an error screen, 0xE0A0AC01); 10BF00 another region's id (a catalogue); Japan 10BE00, Korea
    // 120F00, Taiwan 123200 from the catalogue. Its kiosk demos 122C00 and 122E00 are left out.
    {"Kirby Triple Deluxe", {0x0010C000u, 0x0010BF00u, 0x0010BE00u, 0x00120F00u, 0x00123200u}, kNoRules, {DlpGameMode::Lobby, false, 4}},
    // Kirby Planet Robobot (family AT3): 183600 seen on a console (as Triple Deluxe: GetChannels, scan, join); Korea 17C600 from the catalogue,
    // where 183600 is FREE: one id sold in more than one region - which ones the catalogue does not say, Japan not verified. Its kiosk demos
    // 189600 and 189800 are left out.
    // D6 (2026-11-17): RealDlp with the Azahar - Azahar capture of this game (Tests/dlp_real_robobot_golden.inc, Tests/dlp_real_lobby_games_test.cpp). A console
    // of an earlier build (Lobby) and one of this build (RealDlp) do not see each other in this game; back to Lobby: this one value.
    {"Kirby Planet Robobot", {0x00183600u, 0x0017C600u}, kNoRules, {DlpGameMode::RealDlp, false, 4}},
    // Mario Tennis Open (family AGA): Europe 7C800 (the owner's copy, tried on two New 3DS with the plugin in 1.0.12: its local multiplayer scene
    // starts the Download Play client at once - Initialize, GetMyStatus every 5 ms, GetChannels, scan, join - read from the code), USA 7C700
    // (catalogues and the Azahar list), Japan 64D00 (one source), Taiwan B9100 (one source), Korea B8800 (the catalogue): none but Europe seen. The
    // console's seed: the game compares it (above). Its kiosk demos A2400 and A2500 are left out.
    {"Mario Tennis Open", {0x0007C800u, 0x0007C700u, 0x00064D00u, 0x000B9100u, 0x000B8800u}, kNoRules, {DlpGameMode::Lobby, true, 4}},
    // Mario Kart 7 (family AMK): Europe 30700 seen; USA 30800, Japan 30600, Korea 30A00, Taiwan 8B400 from the catalogue (its two kiosk demos 81500
    // and 81600, and 8B500, which is not identified, are left out). Seen on two consoles with 1.1.3 (2026-11-11): its local multiplayer opens
    // dlp:FKCL and calls InitializeWithName (0x11), and the real module answered 0xC940AC04 (wireless off). Seen on a console with 1.1.4
    // (2026-11-13), then in Lobby mode: the creator calls dlp:SRVR InitializeWithName (0x0F) asking for 7 clients, StartHosting with automatic
    // acceptance, then GetServerState, GetClientInfo and GetConnectingClients every 17 ms; the searcher looks with dlp:FKCL. The game plays with up
    // to eight (host + 7 clients): the only row with a ceiling of its own. In Lobby mode an Azahar player in the room did not see the console's
    // group nor the console an Azahar player's (Azahar speaks the real Download Play). RealDlp since then: the contract is the capture of EIGHT
    // Azahar that played it the same day (analysis/mk7-azahar-2026-11-13: "max 8 nodes", the seven come in one at a time as fake clients and at
    // the final with the passphrase everybody moves to the game's own network of eight; Tests/dlp_real_mk7_golden.inc and
    // Tests/dlp_real_mk7_test.cpp hold the adapter to it, with seven clients). With the console's seed, as the real Download Play gives it: not
    // NEEDED by this game (in the capture every Azahar had the seed 0 and the name "Player" in the Download Play's network, and they raced), and
    // no path found where it does worse than the hash of the MAC but one: two consoles with one seed and one name (a cloned system) that enter a
    // lobby in the same round would adopt the same Logoff. A failed read falls back to the hash and says so. An Azahar that SEARCHES needs a
    // correction of its own to list any host of this game (analysis/mk7-azahar-2026-11-13/ESITO-PROVA-OTTO-AZAHAR.md); a console that searches
    // fills it. Never seen on a console in this mode; a lobby of more than four was never seen on consoles at all: what holds it is the PC's tests.
    {"Mario Kart 7", {0x00030700u, 0x00030800u, 0x00030600u, 0x00030A00u, 0x0008B400u}, kNoRules, {DlpGameMode::RealDlp, true, 8}},
    // Luigi's Mansion (the 2018 remake, family BGN): Europe 1D1A00 seen on a New 3DS XL with 1.1.2 (2026-11-08): its local multiplayer opens the
    // Download Play at once - dlp:SRVR InitializeWithName (0x0F) as the host and dlp:FKCL InitializeWithName (0x11) as the client - and the real
    // modules answered 0xC940AC04 (wireless off); with the lobby it played (two New 3DS, 2026-11-09). USA 1D1900, Japan 1D1800, Korea 1D4F00 from
    // the catalogue. 1.1.3: RealDlp (the Azahar - Azahar capture of this game, analysis/luigi-azahar-2026-11-09, is the contract), so that a
    // console and Azahar play it together; back to Lobby with this one value if the test of two consoles (O5) fails.
    {"Luigi's Mansion", {0x001D1A00u, 0x001D1900u, 0x001D1800u, 0x001D4F00u}, kNoRules, {DlpGameMode::RealDlp, false, 4}},
};
inline constexpr std::size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);
/// The ids of a row's family in the catalogue that the row leaves out on purpose (a demo, a kiosk copy), so that the test of the families knows them:
/// the kiosk demos of Kirby Triple Deluxe (122C00, 122E00), of Kirby Planet Robobot (189600, 189800), of Mario Tennis Open (A2400, A2500) and of
/// Mario Kart 7 (81500, 81600) - demos for shops, read from their CIA headers - and Mario Kart 7's 8B500, which nobody identified.
inline constexpr u32 kLeftOut[] = {0x00122C00u, 0x00122E00u, 0x00189600u, 0x00189800u, 0x000A2400u, 0x000A2500u, 0x00081500u, 0x00081600u, 0x0008B500u};

/// The row of `title_id` (the application or its Download Play child, any region of the row), or -1 for every other title.
constexpr int RowOf(u64 title_id) {
    const u32 high = static_cast<u32>(title_id >> 32);
    const u32 low = static_cast<u32>(title_id);
    if ((high != kTitleHighApplication && high != kTitleHighDlpChild) || low == 0) {
        return -1;
    }
    for (std::size_t r = 0; r < kRowCount; ++r) {
        for (const u32 listed : kRows[r].low) {
            if (listed == low) {
                return static_cast<int>(r);
            }
        }
    }
    return -1;
}

/// The rules `title_id` runs with: its row's, or kNoRules.
constexpr Rules RulesFor(u64 title_id) {
    const int r = RowOf(title_id);
    return r < 0 ? kNoRules : kRows[r].rules;
}

/// The rules of the APPLICATION `title_id`: its row's, or kNoRules - and kNoRules for its Download Play child, an update, anything else. The start-up
/// rules are read with this one (the process's own id), so a child starts as it ever did.
constexpr Rules AppRulesFor(u64 title_id) {
    return static_cast<u32>(title_id >> 32) == kTitleHighApplication ? RulesFor(title_id) : kNoRules;
}
/// The Download Play column of the APPLICATION `title_id` (its row's, or kNoDlp) - kNoDlp for its Download Play child, an update, a system title
/// (RowOf refuses 00040010: the Download Play application is ctr/text_search_guard.hpp's), anything else: what kDlpGames answered (whole ids).
constexpr Dlp DlpOf(u64 title_id) {
    return static_cast<u32>(title_id >> 32) == kTitleHighApplication && RowOf(title_id) >= 0 ? kRows[RowOf(title_id)].dlp : kNoDlp;
}
/// The name of `title_id`'s row, or "" for a title with no row.
constexpr const char* NameOf(u64 title_id) {
    const int r = RowOf(title_id);
    return r < 0 ? "" : kRows[r].name;
}

/// 3DS Link's own title id (DlpLink/tools/common.sh: UNIQUE_ID 0x0F83D1, variation 00), which is in the band of the prepared children: never one.
inline constexpr u64 kAppTid = 0x000400000F83D100ull;
/// The band of the low words of the prepared children: 0x0F800000 | the 23 low bits of the game's low word (the user's decision,
/// analysis/piano-1.1.4-2026-11-15/specifiche/REGOLE-IMPLEMENTAZIONE.md, above the contract's bound: the band 0x0F7xxxxx of the catalogue
/// gets 0x0FFxxxxx; Tests/dlp_children_test.cpp holds that no two sold games share one).
inline constexpr u32 kPreparedBand = 0x0F800000u;
inline constexpr u32 kPreparedBandMask = 0xFF800000u;
inline constexpr u32 kPreparedLowMask = 0x007FFFFFu;

/// The prepared child of the APPLICATION `game` (00040000, low word not 0): 000400000F800000 | the low word's 23 low bits, for every game
/// (the band 0x0F7xxxxx of the catalogue too); 0 = no prepared child is possible for it - an id that is not an application's, or one whose
/// prepared id would be the game itself (a title of the band) or 3DS Link.
constexpr u64 PreparedOf(u64 game) {
    const u32 high = static_cast<u32>(game >> 32);
    const u32 low = static_cast<u32>(game);
    if (high != kTitleHighApplication || low == 0) {
        return 0;
    }
    const u64 prepared = (static_cast<u64>(kTitleHighApplication) << 32) | (kPreparedBand | (low & kPreparedLowMask));
    return (prepared == game || prepared == kAppTid) ? 0 : prepared;
}
/// Is `tid` in the band of the prepared children (an application, low word 0x0F800000..0x0FFFFFFF)? The band only: 3DS Link and the homebrew
/// of 0x0F8xxxxx are in it too, so who a prepared child is says the index's row whose `prepared` it is (azl/dlp_children.hpp).
constexpr bool IsPreparedId(u64 tid) {
    return static_cast<u32>(tid >> 32) == kTitleHighApplication && (static_cast<u32>(tid) & kPreparedBandMask) == kPreparedBand;
}
static_assert(PreparedOf(0x00040000000F8200ull) == 0x000400000F8F8200ull && IsPreparedId(PreparedOf(0x00040000000F8200ull)),
              "Mario Party: Island Tour (Europe) has its prepared child at 000400000F8F8200");
static_assert(PreparedOf(0x00040001000F8200ull) == 0 && PreparedOf(0x000400000F8F8200ull) == 0 && PreparedOf(0x000400000003D100ull) == 0 &&
                  PreparedOf(0x0004000000000000ull) == 0,
              "no prepared child for a child, for a prepared child, for the game whose would be 3DS Link, for no title");
static_assert(PreparedOf(0x000400000F700000ull) == 0x000400000FF00000ull && PreparedOf(0x000400000F715F00ull) == 0x000400000FF15F00ull,
              "the band 0x0F7xxxxx of the catalogue has its prepared children too: the 23 low bits, no bound (the user's decision)");
static_assert(IsPreparedId(kAppTid) && !IsPreparedId(0x000400010F8F8200ull) && !IsPreparedId(0x000400000F700000ull) && !IsPreparedId(0x0004000010000000ull),
              "the band is the application's 0x0F800000..0x0FFFFFFF and nothing else");
/// (the build 2h) the id of the SYSTEM of the prepared child whose key is `prepared` (a key of the band, 00040000|low): the same low word under
/// 00040010 - the one id AM, the ticket, the TMD, the NCCH, the exheader, pm, NS, Luma's loader and Rosalina's folder see. 0 for an id that is not a
/// key of the band. The key itself stays the name of the app's files (the index, the .guest, the .bin): never given to the system in this form.
constexpr u64 PreparedSystemOf(u64 prepared) {
    return IsPreparedId(prepared) ? (static_cast<u64>(kTitleHighSystemApplication) << 32) | static_cast<u32>(prepared) : 0;
}
/// (the build 2h) the key of the prepared child a PROCESS of id `tid` is: the key itself (00040000: the fallback form, the children of the builds
/// up to 2g) or its system id (00040010) gives 00040000|low when the low word is in the band; 0 for every other id - 00040001 never (Luma loads
/// no plugin there), 00040002/00040012, the system's Download Play application (00040010, low 0x0002x100), anything out of the band.
constexpr u64 PreparedSlotOfProcess(u64 tid) {
    const u32 high = static_cast<u32>(tid >> 32);
    const u64 slot = (static_cast<u64>(kTitleHighApplication) << 32) | static_cast<u32>(tid);
    return (high == kTitleHighApplication || high == kTitleHighSystemApplication) && IsPreparedId(slot) ? slot : 0;
}
static_assert(PreparedSlotOfProcess(0x000400100F8F8200ull) == 0x000400000F8F8200ull && PreparedSlotOfProcess(0x000400000F8F8200ull) == 0x000400000F8F8200ull,
              "a process of the system id or of the key is the prepared child of that key");
static_assert(PreparedSlotOfProcess(0x000400010F8F8200ull) == 0 && PreparedSlotOfProcess((0x00040012ull << 32) | 0x0F8F8200u) == 0 &&
                  PreparedSlotOfProcess((static_cast<u64>(kTitleHighSystemApplication) << 32) | 0x0F7FFF00u) == 0 &&
                  PreparedSlotOfProcess((static_cast<u64>(kTitleHighSystemApplication) << 32) | 0x10000000u) == 0 &&
                  PreparedSlotOfProcess(0x000400020F8F8200ull) == 0,
              "00040001 never, 00040012 and 00040002 never, nothing out of the band (the system's Download Play application: Tests/plugin_dlp_test.cpp)");
static_assert(PreparedSystemOf(PreparedOf(0x00040000000F8200ull)) == 0x000400100F8F8200ull && PreparedSystemOf(0x000400010F8F8200ull) == 0 &&
                  PreparedSystemOf(0x000400100F8F8200ull) == 0,
              "Island Tour's prepared child is 000400100F8F8200 to the system; only a key of the band has a system id");

/// Are the two ids the same game (one row, whatever region, application or child)? Never for a title with no row.
constexpr bool SameGame(u64 a, u64 b) {
    const int r = RowOf(a);
    return r >= 0 && r == RowOf(b);
}

/// Is every row within the bounds? Each one names at least one id, its ids are not 0 except the unused ones at the end, no id is in
/// two places, it turns on at least one rule or has a Download Play column, each rule is within its bound, and its Download Play column
/// says nothing a game of its mode cannot use.
constexpr bool RowsAreSound() {
    for (std::size_t r = 0; r < kRowCount; ++r) {
        const Row& row = kRows[r];
        if (row.name == nullptr || row.name[0] == '\0' || row.low[0] == 0) {
            return false;
        }
        bool ended = false;
        for (std::size_t i = 0; i < kMaxRegions; ++i) {
            const u32 low = row.low[i];
            if (low == 0) {
                ended = true;
                continue;
            }
            if (ended) {
                return false; // (an id after an unused place)
            }
            for (std::size_t s = 0; s < kRowCount; ++s) {
                for (std::size_t j = 0; j < kMaxRegions; ++j) {
                    if ((s != r || j != i) && kRows[s].low[j] == low) {
                        return false; // (the same id twice)
                    }
                }
            }
        }
        const Rules& rules = row.rules;
        if (rules.same_frame_ms > kSameFrameMsMax || rules.hand_over_burst < 1 || rules.hand_over_burst > kHandOverBurstMax ||
            rules.cap_drop_max > kCapDropMaxBound) {
            return false;
        }
        if (rules.send_budget_bytes != 0 && (rules.send_budget_bytes < kSendBudgetMin || rules.send_budget_bytes > kSendBudgetMax)) {
            return false; // (a budget below a datagram, or one that is no budget)
        }
        const Dlp& dlp = row.dlp;
        if (dlp.max_players < 2 || dlp.max_players > kDlpPlayersMax) {
            return false; // (a ceiling the lobby's tables cannot hold)
        }
        if ((dlp.mode == DlpGameMode::None || dlp.mode == DlpGameMode::ProbeOnly) && (dlp.console_seed || dlp.max_players != kDlpTitlePlayers)) {
            return false; // (a seed or a ceiling for a game with no lobby)
        }
        if (dlp.max_players != kDlpTitlePlayers && dlp.mode != DlpGameMode::RealDlp) {
            return false; // (a ceiling of its own: the real Download Play only)
        }
        if (rules.same_frame_ms == 0 && rules.hand_over_burst == 1 && rules.cap_drop_max == 0 && !rules.host_keeps_to_its_game &&
            rules.send_budget_bytes == 0 && !rules.worker_off_core1 && !rules.late_soc_block && !rules.soc_guest && row.dlp.mode == DlpGameMode::None) {
            return false; // (a row that turns nothing on)
        }
    }
    return true;
}
static_assert(RowsAreSound(), "a row of azl/title_rules.hpp is out of its bounds: an id missing or twice, or a rule beyond its bound");
static_assert(kNoRules.same_frame_ms == 0 && kNoRules.hand_over_burst == 1 && kNoRules.cap_drop_max == 0 && !kNoRules.host_keeps_to_its_game &&
                  kNoRules.send_budget_bytes == 0 && !kNoRules.worker_off_core1 && !kNoRules.late_soc_block && !kNoRules.soc_guest,
              "a game with no row runs with every rule off");
static_assert(kNoDlp.mode == DlpGameMode::None && !kNoDlp.console_seed && kNoDlp.max_players == kDlpTitlePlayers,
              "a game with no Download Play column keeps the real Download Play services");

} // namespace title_rules
} // namespace azl
