// The Download Play wiring of Sources/ctr/plugin.cpp on the PC: the table of title ids and modes (DlpModeOf, AsksDlpOnlyForTheWireless),
// the friend code seed (Fnv1a64), the resolver of the game's buffers (MapGameAddress) and the hub's configuration (MakeDlpHub, and 1.1.3's
// MakeRealDlpHub). plugin.cpp cannot be built on the PC (libctrpf), so this test reads it: plugin_dlp_extract.py cuts the text from
// DlpModeOf to the end of MakeRealDlpHub out of plugin.cpp into plugin_dlp_slice.inc, and this file compiles that text, unchanged, in the
// namespace it lives in. (Mutation round of phase 3, label "mutants": none of these lines was run by any test; Tests/policy_check.sh could only pin
// their text.) 1.1.3, phase F5: the extractor also cuts the block of Plugin::EarlyInit that decides, by the table, which hub a game gets
// (plugin_dlp_wiring.inc), and WiringTest runs it, unchanged, inside a stand-in of the plugin for EVERY row of the table, for games that are not
// in it and with the plugin inactive: only the RealDlp rows (Luigi's Mansion) get the adapter of the real Download Play; every other row and
// every other game gets exactly what it got before (a Lobby hub on the hub's own adapter, or nothing) - R11 of PROGETTO-DLP-VERO-1.1.3.md.
// 1.1.4, L4: the table is the Download Play column of Includes/azl/title_rules.hpp (the slice's functions read it through title_rules::DlpOf);
// kDlpGames below is a view of it in the shape of the plugin's old table, and AZL_DLP_AS_BEFORE holds every one of the thirty ids to what
// the old table answered, at compile time.
//   python3 Tests/plugin_dlp_extract.py Sources/ctr/plugin.cpp "$OUT/plugin_dlp_slice.inc"
//   g++ -std=c++17 -O1 -g -Wall -Wextra -IIncludes -I"$OUT" -pthread -fsanitize=address,undefined
//       Tests/plugin_dlp_test.cpp Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp
//       dlph_wire.o <CORE> <enet objects>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#define private public  // (the test reads the lobby's identity: who the hub made it, no other way to see the configuration)
#define protected public
#include "azl/dlp_hub.hpp"
#include "azl/dlp_real_net.hpp"
#undef private
#undef protected
#include "azl/title_rules.hpp"
#include "ctr/text_search_guard.hpp"  // (1.1.4, G1: DlpModeOf says SystemClient for the six ids of the Download Play application)

namespace azl {
namespace ctr {
namespace {
#include "plugin_dlp_slice.inc"
}  // namespace

// The Download Play column of azl/title_rules.hpp as the plugin's table was until L4: one entry per APPLICATION id of a row that has the
// column, in the table's order (row by row, ids in the row's order).
struct DlpGame { u64 title_id; DlpGameMode mode; bool console_seed; u8 max_players; };
constexpr std::size_t DlpIdCount() {
    std::size_t n = 0;
    for (std::size_t r = 0; r < title_rules::kRowCount; ++r) {
        for (const u32 low : title_rules::kRows[r].low) {
            n += (low != 0 && title_rules::kRows[r].dlp.mode != DlpGameMode::None) ? 1u : 0u;
        }
    }
    return n;
}
constexpr std::array<DlpGame, DlpIdCount()> DlpView() {
    std::array<DlpGame, DlpIdCount()> view = {};
    std::size_t at = 0;
    for (std::size_t r = 0; r < title_rules::kRowCount; ++r) {
        const title_rules::Dlp& dlp = title_rules::kRows[r].dlp;
        for (const u32 low : title_rules::kRows[r].low) {
            if (low == 0 || dlp.mode == DlpGameMode::None) {
                continue;
            }
            view[at] = DlpGame{0x0004000000000000ull | low, dlp.mode, dlp.console_seed, dlp.max_players};
            ++at;
        }
    }
    return view;
}
static constexpr auto kDlpGames = DlpView();
static_assert(kDlpGames.size() == 30, "the thirty ids of the Download Play column");

// Every one of the thirty ids answers what the plugin's table answered before L4 - the mode, the seed flag, the ceiling - and its Download Play
// child (00040001) has no column, as the old table compared whole ids. Written by hand, in the order of the old table.
#define AZL_DLP_AS_BEFORE(tid, m, seed, players)                                                                         \
    static_assert(title_rules::DlpOf(tid).mode == DlpGameMode::m && title_rules::DlpOf(tid).console_seed == (seed) &&     \
                      title_rules::DlpOf(tid).max_players == (players) &&                                                 \
                      title_rules::DlpOf((tid) | 0x0000000100000000ull).mode == DlpGameMode::None,                       \
                  #tid " keeps its Download Play")
AZL_DLP_AS_BEFORE(0x00040000001C2100ull, ProbeOnly, false, 4);
AZL_DLP_AS_BEFORE(0x00040000001C2000ull, ProbeOnly, false, 4);
AZL_DLP_AS_BEFORE(0x00040000001A8B00ull, ProbeOnly, false, 4);
AZL_DLP_AS_BEFORE(0x00040000001C2200ull, ProbeOnly, false, 4);
AZL_DLP_AS_BEFORE(0x000400000010C000ull, Lobby, false, 4);
AZL_DLP_AS_BEFORE(0x000400000010BF00ull, Lobby, false, 4);
AZL_DLP_AS_BEFORE(0x000400000010BE00ull, Lobby, false, 4);
AZL_DLP_AS_BEFORE(0x0004000000120F00ull, Lobby, false, 4);
AZL_DLP_AS_BEFORE(0x0004000000123200ull, Lobby, false, 4);
AZL_DLP_AS_BEFORE(0x0004000000183600ull, RealDlp, false, 4);
AZL_DLP_AS_BEFORE(0x000400000017C600ull, RealDlp, false, 4);
AZL_DLP_AS_BEFORE(0x000400000007C800ull, Lobby, true, 4);
AZL_DLP_AS_BEFORE(0x000400000007C700ull, Lobby, true, 4);
AZL_DLP_AS_BEFORE(0x0004000000064D00ull, Lobby, true, 4);
AZL_DLP_AS_BEFORE(0x00040000000B9100ull, Lobby, true, 4);
AZL_DLP_AS_BEFORE(0x00040000000B8800ull, Lobby, true, 4);
AZL_DLP_AS_BEFORE(0x0004000000030700ull, RealDlp, true, 8);
AZL_DLP_AS_BEFORE(0x0004000000030800ull, RealDlp, true, 8);
AZL_DLP_AS_BEFORE(0x0004000000030600ull, RealDlp, true, 8);
AZL_DLP_AS_BEFORE(0x0004000000030A00ull, RealDlp, true, 8);
AZL_DLP_AS_BEFORE(0x000400000008B400ull, RealDlp, true, 8);
AZL_DLP_AS_BEFORE(0x00040000001D1A00ull, RealDlp, false, 4);
AZL_DLP_AS_BEFORE(0x00040000001D1900ull, RealDlp, false, 4);
AZL_DLP_AS_BEFORE(0x00040000001D1800ull, RealDlp, false, 4);
AZL_DLP_AS_BEFORE(0x00040000001D4F00ull, RealDlp, false, 4);
AZL_DLP_AS_BEFORE(0x00040000000F8200ull, RealDlp, false, 4);
AZL_DLP_AS_BEFORE(0x00040000000F8100ull, RealDlp, false, 4);
AZL_DLP_AS_BEFORE(0x00040000000DD400ull, RealDlp, false, 4);
AZL_DLP_AS_BEFORE(0x0004000000119300ull, RealDlp, false, 4);
AZL_DLP_AS_BEFORE(0x0004000000119B00ull, RealDlp, false, 4);

static int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) {                                                                    \
            ++g_failures;                                                                 \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                                 \
    } while (0)
#define CHECK_EQ(a, b)                                                                                                  \
    do {                                                                                                                \
        ++g_checks;                                                                                                     \
        const unsigned long long va_ = static_cast<unsigned long long>(a), vb_ = static_cast<unsigned long long>(b);    \
        if (va_ != vb_) {                                                                                               \
            ++g_failures;                                                                                               \
            std::fprintf(stderr, "  FAIL %s:%d: %s == %s (0x%llx vs 0x%llx)\n", __FILE__, __LINE__, #a, #b, va_, vb_);  \
        }                                                                                                               \
    } while (0)

struct FakeKernel final : KernelOps {
    u32 CreateEvent() override { return 5; }
    u32 DuplicateForGame(u32 h) override { return h; }
    void SignalEvent(u32) override {}
    void ClearEvent(u32) override {}
    bool WaitEvent(u32, s64) override { return false; }
    void CloseEvent(u32) override {}
    void SleepNs(s64) override {}
};

static void TableTest() {
    std::printf("plugin/the table of title ids: exactly these thirty, in this order, nothing else (Luigi's Mansion, Mario Kart 7 and Mario Party: Island Tour in RealDlp; every region of every game)\n");
    struct Want {
        u64 tid;
        DlpGameMode mode;
        bool console_seed;
        u8 max_players;  // the ceiling of the title's lobby, host included: 4 for every game that played before the lobby held eight, 8 for Mario Kart 7
    };
    // written out by hand, in the order of kDlpGames, the view of the Download Play column of azl/title_rules.hpp (row by row: Mario Party:
    // Island Tour is the table's first row; the mode, the seed flag and the ceiling of players of every title the plugin knows about; the
    // comments of the rows say which are seen on a console). Whoever adds, moves or removes an id of the column changes this table in the
    // same commit.
    static constexpr Want want[] = {
        {0x00040000000DD400ull, DlpGameMode::RealDlp, false, 4},    // Mario Party: Island Tour, Japan (from the catalogue)
        {0x00040000000F8100ull, DlpGameMode::RealDlp, false, 4},    // Mario Party: Island Tour, USA (from the catalogue: the values of the European row)
        {0x00040000000F8200ull, DlpGameMode::RealDlp, false, 4},    // Mario Party: Island Tour, Europe (2026-11-14: the console hosts, the real Download Play)
        {0x0004000000119300ull, DlpGameMode::RealDlp, false, 4},    // Mario Party: Island Tour, Taiwan (from the catalogue)
        {0x0004000000119B00ull, DlpGameMode::RealDlp, false, 4},    // Mario Party: Island Tour, Korea (from the catalogue)
        {0x00040000001C2100ull, DlpGameMode::ProbeOnly, false, 4},  // Kirby Battle Royale
        {0x00040000001C2000ull, DlpGameMode::ProbeOnly, false, 4},  // Kirby Battle Royale, another region
        {0x00040000001A8B00ull, DlpGameMode::ProbeOnly, false, 4},  // Kirby Battle Royale, Japan (2026-11-14: every region of a game, from the catalogue)
        {0x00040000001C2200ull, DlpGameMode::ProbeOnly, false, 4},  // Kirby Battle Royale, Korea (from the catalogue)
        {0x000400000010C000ull, DlpGameMode::Lobby, false, 4},      // Kirby Triple Deluxe
        {0x000400000010BF00ull, DlpGameMode::Lobby, false, 4},      // Kirby Triple Deluxe, another region
        {0x000400000010BE00ull, DlpGameMode::Lobby, false, 4},      // Kirby Triple Deluxe, Japan (from the catalogue)
        {0x0004000000120F00ull, DlpGameMode::Lobby, false, 4},      // Kirby Triple Deluxe, Korea (from the catalogue)
        {0x0004000000123200ull, DlpGameMode::Lobby, false, 4},      // Kirby Triple Deluxe, Taiwan (from the catalogue)
        {0x0004000000183600ull, DlpGameMode::RealDlp, false, 4},    // Kirby Planet Robobot
        {0x000400000017C600ull, DlpGameMode::RealDlp, false, 4},    // Kirby Planet Robobot, Korea (from the catalogue)
        {0x000400000007C800ull, DlpGameMode::Lobby, true, 4},       // Mario Tennis Open, Europe
        {0x000400000007C700ull, DlpGameMode::Lobby, true, 4},       // Mario Tennis Open, USA
        {0x0004000000064D00ull, DlpGameMode::Lobby, true, 4},       // Mario Tennis Open, Japan
        {0x00040000000B9100ull, DlpGameMode::Lobby, true, 4},       // Mario Tennis Open, Taiwan
        {0x00040000000B8800ull, DlpGameMode::Lobby, true, 4},       // Mario Tennis Open, Korea (from the catalogue: the console's seed, as the game's other rows)
        {0x0004000000030700ull, DlpGameMode::RealDlp, true, 8},     // Mario Kart 7, Europe (2026-11-13: eight players, its creator asks for 7 clients; the real Download Play)
        {0x0004000000030800ull, DlpGameMode::RealDlp, true, 8},     // Mario Kart 7, USA
        {0x0004000000030600ull, DlpGameMode::RealDlp, true, 8},     // Mario Kart 7, Japan
        {0x0004000000030A00ull, DlpGameMode::RealDlp, true, 8},     // Mario Kart 7, Korea
        {0x000400000008B400ull, DlpGameMode::RealDlp, true, 8},     // Mario Kart 7, Taiwan
        {0x00040000001D1A00ull, DlpGameMode::RealDlp, false, 4},    // Luigi's Mansion (2018), Europe (1.1.3: the real Download Play)
        {0x00040000001D1900ull, DlpGameMode::RealDlp, false, 4},    // Luigi's Mansion (2018), USA
        {0x00040000001D1800ull, DlpGameMode::RealDlp, false, 4},    // Luigi's Mansion (2018), Japan
        {0x00040000001D4F00ull, DlpGameMode::RealDlp, false, 4},    // Luigi's Mansion (2018), Korea
    };
    constexpr std::size_t kRows = kDlpGames.size();
    constexpr std::size_t kWant = sizeof(want) / sizeof(want[0]);
    CHECK_EQ(kRows, kWant);
    // row by row: the same title, the same mode and the same seed flag (the size check alone would not see two rows swapped or one replaced)
    for (std::size_t i = 0; i < kRows && i < kWant; ++i) {
        CHECK_EQ(kDlpGames[i].title_id, want[i].tid);
        CHECK(kDlpGames[i].mode == want[i].mode);
        CHECK_EQ(kDlpGames[i].console_seed, want[i].console_seed);
        CHECK_EQ(kDlpGames[i].max_players, want[i].max_players);
    }
    // 1.1.4 (G1, K5): the six ids of the system's Download Play application are SystemClient (no row: DlpModeOf's first line), every region;
    // their neighbours, the application's own id as an application, and a child are not
    u32 system_clients = 0;
    for (const u64 t : kDownloadPlayTitles) {
        CHECK(DlpModeOf(t) == DlpGameMode::SystemClient);
        CHECK(title_rules::DlpOf(t).mode == DlpGameMode::None);  // (the table knows nothing of it: the mode is the plugin's)
        CHECK(!LobbySeedIsConsoles(t) && !AsksDlpOnlyForTheWireless(t) && !AdmissionToMembersOf(t));
        system_clients += DlpModeOf(t) == DlpGameMode::SystemClient ? 1u : 0u;
    }
    CHECK_EQ(system_clients, 6u);
    for (const u64 t : {0x0004001000022000ull, 0x0004001000022101ull, 0x0004000000022100ull, 0x0004001100022100ull, 0x0004001000022200ull,
                        0x0004001000020000ull, 0x0004001000021101ull}) {
        CHECK(DlpModeOf(t) == DlpGameMode::None);
    }
    for (std::size_t i = 0; i < kRows; ++i) {
        CHECK(DlpModeOf(kDlpGames[i].title_id) != DlpGameMode::SystemClient);  // (no game of the table is the application)
    }
    // no title twice (DlpModeOf and LobbySeedIsConsoles take the first row: a second one would be dead text that looks alive)
    for (std::size_t i = 0; i < kRows; ++i) {
        for (std::size_t j = i + 1; j < kRows; ++j) {
            CHECK(kDlpGames[i].title_id != kDlpGames[j].title_id);
        }
    }
    for (const Want& w : want) {
        CHECK(DlpModeOf(w.tid) == w.mode);
        CHECK_EQ(AsksDlpOnlyForTheWireless(w.tid), w.mode == DlpGameMode::ProbeOnly);
        // the column is the APPLICATION's: its Download Play child (00040001, one row with it in title_rules) has none, as in the old table
        CHECK(title_rules::DlpOf(w.tid | 0x0000000100000000ull).mode == DlpGameMode::None);
        CHECK(title_rules::RowOf(w.tid | 0x0000000100000000ull) == title_rules::RowOf(w.tid));
        CHECK(DlpModeOf(w.tid | 0x0000000100000000ull) == DlpGameMode::None && !LobbySeedIsConsoles(w.tid | 0x0000000100000000ull));
        CHECK_EQ(LobbyPlayersOf(w.tid | 0x0000000100000000ull), 4u);
        // the console's own friend code seed only where the row says so (Mario Tennis Open, Mario Kart 7), and only for a game that has a hub
        CHECK_EQ(LobbySeedIsConsoles(w.tid), w.console_seed);
        if (w.console_seed) {
            CHECK(DlpModeOf(w.tid) == DlpGameMode::Lobby || DlpModeOf(w.tid) == DlpGameMode::RealDlp);
            CHECK(w.max_players == 4 || DlpModeOf(w.tid) == DlpGameMode::RealDlp);  // Mario Tennis Open: four; Mario Kart 7: RealDlp, eight
        }
        // the ceiling of players is the row's, and it is 4 or 8 and nothing else
        CHECK_EQ(LobbyPlayersOf(w.tid), w.max_players);
        CHECK(w.max_players == 4 || w.max_players == 8);
        // a title id that differs in ONE bit is another game: never in the table, never the console's seed
        for (int bit = 0; bit < 64; ++bit) {
            const u64 other = w.tid ^ (1ull << bit);
            bool in_table = false;
            for (const Want& x : want) {
                in_table |= x.tid == other;
            }
            if (!in_table) {
                CHECK(DlpModeOf(other) == DlpGameMode::None);
                CHECK(!AsksDlpOnlyForTheWireless(other));
                CHECK(!LobbySeedIsConsoles(other));
                CHECK_EQ(LobbyPlayersOf(other), 4u);
                CHECK(!AdmissionToMembersOf(other));
            }
        }
    }
    // exactly the five Mario Kart 7 ids have a ceiling of eight players, written here once more by hand; every other row - Kirby Triple Deluxe,
    // Planet Robobot, Mario Tennis Open, Kirby Battle Royale, Luigi's Mansion - has four, the default of a row that does not say it
    static const u64 eights[] = {0x0004000000030700ull, 0x0004000000030800ull, 0x0004000000030600ull, 0x0004000000030A00ull, 0x000400000008B400ull};
    std::size_t eight_rows = 0;
    for (const DlpGame& game : kDlpGames) {
        bool is_mk7 = false;
        for (const u64 t : eights) {
            is_mk7 |= game.title_id == t;
        }
        CHECK_EQ(game.max_players, is_mk7 ? 8u : 4u);
        CHECK_EQ(LobbyPlayersOf(game.title_id), is_mk7 ? 8u : 4u);
        // ... and exactly those five have the game's own network admit to its members one by one (the engine's switch of the title): Luigi's
        // Mansion, the Kirbys, Mario Tennis Open and Kirby Battle Royale broadcast the Logoff of an admission as ever
        CHECK_EQ(AdmissionToMembersOf(game.title_id), is_mk7);
        if (game.max_players != 4) {
            ++eight_rows;
            CHECK(game.mode == DlpGameMode::RealDlp);  // (Mario Kart 7 since 2026-11-13: the real Download Play, whose adapter holds host + 7 clients)
        }
    }
    CHECK_EQ(eight_rows, 5u);
    CHECK_EQ(title_rules::kNoDlp.max_players, 4u);  // a game with no column: four
    CHECK(title_rules::kNoDlp.mode == DlpGameMode::None && !title_rules::kNoDlp.console_seed);
    CHECK_EQ(kTitlePlayers, 4u);
    CHECK_EQ(LobbyPlayersOf(0), 4u);
    // exactly the five Mario Tennis Open ids (the game matches its lobby players with their nwm::UDS nodes by it; Korea's since 2026-11-14) and the five of
    // Mario Kart 7 (not known: it gets what a real console gives) have the console's seed; Kirby and Luigi's Mansion (which do not
    // match them) keep the hash of the MAC
    std::size_t seeded = 0;
    for (const DlpGame& game : kDlpGames) {
        seeded += game.console_seed ? 1u : 0u;
    }
    CHECK_EQ(seeded, 10u);
    CHECK(!LobbySeedIsConsoles(0));
    // games that must keep the real services (Tri Force Heroes, Mario Party Star Rush, Fire Emblem Fates, Pokemon, the empty id), and
    // the ids that share a family of the catalogue with a game of the table and stay out of it: the kiosk demos of Kirby Triple Deluxe (122C00,
    // 122E00), of Kirby Planet Robobot (189600, 189800) and of Mario Tennis Open (A2400, A2500), the two of Mario Kart 7 and the id of its
    // family that is not identified (8B500), and Tetris
    // (4C100), which plays in eight with the game's own nwm::UDS network and never asks for the Download Play
    static const u64 others[] = {0, 0x0004000000164800ull, 0x0004000000199300ull, 0x000400000017E600ull, 0x0004000000055D00ull, 0x00040000001B5100ull,
                                 0x000400000010C100ull, 0x0004000000183700ull, 0x00040000001C2300ull, 0xFFFFFFFFFFFFFFFFull, 0x000400000004C100ull,
                                 0x0004000000122C00ull, 0x0004000000122E00ull, 0x0004000000189600ull, 0x0004000000189800ull, 0x00040000000A2400ull, 0x00040000000A2500ull,
                                 0x0004000000081500ull, 0x0004000000081600ull, 0x000400000008B500ull};
    for (const u64 t : others) {
        CHECK(DlpModeOf(t) == DlpGameMode::None);
        CHECK(DlpModeOf(t | 0x0000000100000000ull) == DlpGameMode::None);  // (nor its child)
        CHECK(!AsksDlpOnlyForTheWireless(t));
        CHECK(!LobbySeedIsConsoles(t));
        CHECK_EQ(LobbyPlayersOf(t), 4u);
        CHECK(!AdmissionToMembersOf(t));
    }
    CHECK(DlpModeOf(0x000400000004C100ull) == DlpGameMode::None);  // Tetris: eight players on its own nwm::UDS network ("max 8 nodes" on a console), no row here
    // the hub exists only for Lobby and RealDlp: ProbeOnly and None have none (WiringTest runs the plugin's own decision)
    CHECK(DlpModeOf(0x00040000001C2100ull) != DlpGameMode::Lobby);
    // the RealDlp ids, written here once more by hand: the four of Luigi's Mansion (the hash of the MAC, four players), the five of Mario
    // Kart 7 (the console's seed, eight players), the five of Mario Party: Island Tour (the hash of the MAC, four players: the European one,
    // whose code was read, and since 2026-11-14 its other four regions with the same values), and `d6_real` below
    static const u64 luigis[] = {0x00040000001D1A00ull, 0x00040000001D1900ull, 0x00040000001D1800ull, 0x00040000001D4F00ull};
    static const u64 island = 0x00040000000F8200ull;
    static const u64 islands[] = {0x00040000000F8200ull, 0x00040000000F8100ull, 0x00040000000DD400ull, 0x0004000000119300ull, 0x0004000000119B00ull};
    // D6 (1.1.4): the rows of the games whose players all have the game that moved from Lobby to RealDlp, each with its own Azahar - Azahar
    // capture (Tests/dlp_real_lobby_games_test.cpp holds the adapter to it), every region of the row; four players, the seed the row says
    static const u64 d6_real[] = {0x0004000000183600ull, 0x000400000017C600ull};    // (0 is no title: a list of 0 alone is empty)
    static const u64 d6_seeded[] = {0};  // ... those of them whose row has the console's seed
    std::size_t real = 0, real_luigi = 0, real_mk7 = 0, real_island = 0, real_d6 = 0;
    for (const DlpGame& game : kDlpGames) {
        bool is_luigi = false, is_mk7 = false;
        for (const u64 t : luigis) {
            is_luigi |= game.title_id == t;
        }
        for (const u64 t : eights) {
            is_mk7 |= game.title_id == t;
        }
        bool is_island = false;
        for (const u64 t : islands) {
            is_island |= game.title_id == t;
        }
        bool is_d6 = false, is_d6_seeded = false;
        for (const u64 t : d6_real) {
            is_d6 |= game.title_id == t;
        }
        for (const u64 t : d6_seeded) {
            is_d6_seeded |= game.title_id == t;
        }
        CHECK_EQ(game.mode == DlpGameMode::RealDlp, is_luigi || is_mk7 || is_island || is_d6);
        if (game.mode == DlpGameMode::RealDlp) {
            ++real;
            real_luigi += is_luigi ? 1u : 0u;
            real_mk7 += is_mk7 ? 1u : 0u;
            real_island += is_island ? 1u : 0u;
            real_d6 += is_d6 ? 1u : 0u;
            CHECK_EQ(game.console_seed, is_mk7 || is_d6_seeded);
            CHECK_EQ(game.max_players, is_mk7 ? 8u : 4u);
            CHECK((game.title_id & 0xFFFFFFFF00000000ull) == 0x0004000000000000ull);
        }
    }
    CHECK_EQ(real, 16u);
    CHECK_EQ(real_luigi, 4u);
    CHECK_EQ(real_mk7, 5u);
    CHECK_EQ(real_island, 5u);
    CHECK_EQ(real_d6, 2u);
    // ... and Mario Party: Island Tour by name: the real Download Play, the hash of the MAC, four players, the admissions of its own network broadcast
    CHECK(DlpModeOf(island) == DlpGameMode::RealDlp);
    CHECK(!LobbySeedIsConsoles(island));
    CHECK_EQ(LobbyPlayersOf(island), 4u);
    CHECK(!AdmissionToMembersOf(island));
    // ... and every region of it the same: what the European row has, the American, the Japanese, the Taiwanese and the Korean one have
    CHECK_EQ(islands[0], island);
    for (const u64 region : islands) {
        CHECK(DlpModeOf(region) == DlpModeOf(island));  // every region of Mario Party: Island Tour: the mode of the European row
        CHECK_EQ(LobbySeedIsConsoles(region), LobbySeedIsConsoles(island));
        CHECK_EQ(LobbyPlayersOf(region), LobbyPlayersOf(island));
        CHECK_EQ(AdmissionToMembersOf(region), AdmissionToMembersOf(island));
        CHECK(!AsksDlpOnlyForTheWireless(region));
    }
    // EVERY REGION OF A GAME (2026-11-14): the games of the table, each with every id it is sold with, written here by hand from the catalogue (family by
    // family; the kiosk demos and the unidentified id are in `others` above). The first id of a game is the one that was seen; every other id of the game has a
    // row with the same mode, the same seed flag and the same ceiling - a region is never given another mode than its game's - and the table has no row
    // that is not an id of one of these games.
    struct Family {
        const char* game;
        u64 ids[5];  // (0 = no more)
    };
    static const Family families[] = {
        {"Kirby Battle Royale", {0x00040000001C2100ull, 0x00040000001C2000ull, 0x00040000001A8B00ull, 0x00040000001C2200ull, 0}},  // AJ8: Europe, the id sold in more than one region, Japan, Korea
        {"Kirby Triple Deluxe", {0x000400000010C000ull, 0x000400000010BF00ull, 0x000400000010BE00ull, 0x0004000000120F00ull, 0x0004000000123200ull}},  // BAL: Europe, USA, Japan, Korea, Taiwan
        {"Kirby Planet Robobot", {0x0004000000183600ull, 0x000400000017C600ull, 0, 0, 0}},  // AT3: the id sold in more than one region (FREE in the catalogue; Japan not verified), Korea
        {"Mario Tennis Open", {0x000400000007C800ull, 0x000400000007C700ull, 0x0004000000064D00ull, 0x00040000000B9100ull, 0x00040000000B8800ull}},  // AGA: Europe, USA, Japan, Taiwan, Korea
        {"Mario Kart 7", {0x0004000000030700ull, 0x0004000000030800ull, 0x0004000000030600ull, 0x0004000000030A00ull, 0x000400000008B400ull}},  // AMK: Europe, USA, Japan, Korea, Taiwan
        {"Luigi's Mansion", {0x00040000001D1A00ull, 0x00040000001D1900ull, 0x00040000001D1800ull, 0x00040000001D4F00ull, 0}},  // BGN: Europe, USA, Japan, Korea
        {"Mario Party: Island Tour", {0x00040000000F8200ull, 0x00040000000F8100ull, 0x00040000000DD400ull, 0x0004000000119300ull, 0x0004000000119B00ull}},  // ATS: Europe, USA, Japan, Taiwan, Korea
    };
    std::size_t family_ids = 0;
    for (const Family& f : families) {
        const u64 seen = f.ids[0];
        CHECK(DlpModeOf(seen) != DlpGameMode::None);
        for (const u64 region : f.ids) {
            if (region == 0) {
                continue;
            }
            ++family_ids;
            CHECK(DlpModeOf(region) == DlpModeOf(seen));  // a region of a game: the mode of the game
            CHECK_EQ(LobbySeedIsConsoles(region), LobbySeedIsConsoles(seen));  // ... its seed
            CHECK_EQ(LobbyPlayersOf(region), LobbyPlayersOf(seen));  // ... its ceiling of players
            CHECK_EQ(AdmissionToMembersOf(region), AdmissionToMembersOf(seen));
            CHECK_EQ(AsksDlpOnlyForTheWireless(region), AsksDlpOnlyForTheWireless(seen));
        }
    }
    CHECK_EQ(family_ids, kRows);  // every row of the table is an id of one of the seven games, and every id of them has a row
    CHECK_EQ(family_ids, 30u);
    for (const DlpGame& game : kDlpGames) {
        bool in_a_family = false;
        for (const Family& f : families) {
            for (const u64 region : f.ids) {
                in_a_family |= region != 0 && region == game.title_id;
            }
        }
        CHECK(in_a_family);
    }
    CHECK(static_cast<u8>(DlpGameMode::RealDlp) == 3);  // (a new value: the old ones keep theirs)
    CHECK(static_cast<u8>(DlpGameMode::Lobby) == 2 && static_cast<u8>(DlpGameMode::ProbeOnly) == 1 && static_cast<u8>(DlpGameMode::None) == 0);
}

static void SeedTest() {
    std::printf("plugin/Fnv1a64: the published vectors (the lobby's friend code seed is the hash of the console's MAC)\n");
    CHECK_EQ(Fnv1a64(reinterpret_cast<const u8*>(""), 0), 0xCBF29CE484222325ull);
    CHECK_EQ(Fnv1a64(reinterpret_cast<const u8*>("a"), 1), 0xAF63DC4C8601EC8Cull);
    CHECK_EQ(Fnv1a64(reinterpret_cast<const u8*>("foobar"), 6), 0x85944171F73967E8ull);
    const u8 mac1[6] = {0x00, 0x1F, 0x32, 0x12, 0x34, 0x56};
    const u8 mac2[6] = {0x00, 0x1F, 0x32, 0x12, 0x34, 0x57};
    CHECK(Fnv1a64(mac1, 6) != Fnv1a64(mac2, 6));
    CHECK(Fnv1a64(mac1, 6) != Fnv1a64(mac1, 5));
}

static void MapTest() {
    std::printf("plugin/MapGameAddress: [0x00100000, 0x40000000), no wrap, null refused\n");
    auto at = [](u32 address, u32 size) { return MapGameAddress(nullptr, address, size); };
    auto ptr = [](u32 address) { return reinterpret_cast<u8*>(static_cast<std::uintptr_t>(address)); };
    CHECK(at(0, 4) == nullptr);
    CHECK(at(0x000FFFFF, 1) == nullptr);
    CHECK(at(0x00000010, 1) == nullptr);
    CHECK(at(0x00100000, 1) == ptr(0x00100000));
    CHECK(at(0x00100000, 0) == ptr(0x00100000));
    CHECK(at(0x08000000, 0x40) == ptr(0x08000000));
    CHECK(at(0x3FFFFFFF, 1) == ptr(0x3FFFFFFF));
    CHECK(at(0x3FFFFFFF, 2) == nullptr);
    CHECK(at(0x3FFFFFF0, 0x10) == ptr(0x3FFFFFF0));
    CHECK(at(0x3FFFFFF0, 0x11) == nullptr);
    CHECK(at(0x40000000, 0) == nullptr);
    CHECK(at(0x40000000, 1) == nullptr);
    CHECK(at(0x00100000, 0x3FF00000) == ptr(0x00100000));
    CHECK(at(0x00100000, 0x3FF00001) == nullptr);
    CHECK(at(0xFFFFFFF0, 0x20) == nullptr);   // wraps
    CHECK(at(0x80000000, 4) == nullptr);      // far past the application's range
    CHECK(at(0x08000000, 0xFFFFFFFF) == nullptr);
}

static void HubTest() {
    std::printf("plugin/MakeDlpHub: the hub is made, in the configuration the plugin gives it\n");
    FakeKernel kernel;
    const Mac mac = Mac{0x00, 0x1F, 0x32, 0xAB, 0xCD, 0xEF};
    const u64 tid = 0x000400000010C000ull;
    std::unique_ptr<DlpHub> hub = MakeDlpHub(nullptr, &kernel, tid, mac);
    CHECK(hub != nullptr);
    if (hub == nullptr) {
        return;
    }
    CHECK(!hub->EngineHeld());
    CHECK_EQ(hub->lobby_.tid_low_, 0x0010C000u);
    CHECK_EQ(hub->lobby_.id_.seed, Fnv1a64(mac.data(), mac.size()));
    CHECK(hub->lobby_.id_.seed != 0);
    static const char kName[] = "3DS Link";
    for (std::size_t i = 0; i < kNameUnits; ++i) {
        CHECK_EQ(hub->lobby_.id_.name[i], i < sizeof(kName) - 1 ? static_cast<u16>(kName[i]) : 0u);
    }
    // the resolver is the plugin's: an address below the code is refused, one inside the application's range is the pointer itself
    CHECK(hub->resolver_.map == &MapGameAddress);
    CHECK(hub->resolver_.map(hub->resolver_.ctx, 0x1000, 4) == nullptr);
    CHECK(hub->resolver_.map(hub->resolver_.ctx, 0x08000000, 4) != nullptr);
    // another console, another seed; the same console, the same seed
    const Mac mac2 = Mac{0x00, 0x1F, 0x32, 0xAB, 0xCD, 0xF0};
    std::unique_ptr<DlpHub> hub2 = MakeDlpHub(nullptr, &kernel, tid, mac2);
    CHECK(hub2 != nullptr && hub2->lobby_.id_.seed != hub->lobby_.id_.seed);
    std::unique_ptr<DlpHub> hub3 = MakeDlpHub(nullptr, &kernel, 0x0004000000183600ull, mac);
    CHECK(hub3 != nullptr && hub3->lobby_.id_.seed == hub->lobby_.id_.seed);
    CHECK(hub3 != nullptr && hub3->lobby_.tid_low_ == 0x00183600u);
    // the Lobby hub runs on its own adapter, as before 1.1.3
    CHECK(hub->lobby_.net() == &hub->net_);
    CHECK(hub->lobby_.net_ == &hub->net_);
    // the ceiling of players reaches the lobby: four for Kirby (Triple Deluxe, Planet Robobot) and for a game with no row, eight for Mario Kart 7
    CHECK_EQ(hub->lobby_.max_players(), 4u);
    CHECK_EQ(hub3->lobby_.max_players(), 4u);
    std::unique_ptr<DlpHub> mk7 = MakeDlpHub(nullptr, &kernel, 0x0004000000030700ull, mac);
    std::unique_ptr<DlpHub> stranger = MakeDlpHub(nullptr, &kernel, 0x0004000000164800ull, mac);
    CHECK(mk7 != nullptr && stranger != nullptr);
    if (mk7 != nullptr && stranger != nullptr) {
        CHECK_EQ(mk7->lobby_.max_players(), 8u);
        CHECK_EQ(stranger->lobby_.max_players(), 4u);
        // ... and it is what cuts the clients a game asks for (Mario Kart 7 asks for 7: dlp:SRVR 0x0F, second word)
        CHECK_EQ(mk7->lobby_.ServerInitialize(7, 0, nullptr), 0u);
        CHECK_EQ(mk7->lobby_.max_clients_, 7u);
        CHECK_EQ(hub->lobby_.ServerInitialize(7, 0, nullptr), 0u);
        CHECK_EQ(hub->lobby_.max_clients_, 3u);
    }
}

static UdsEngine* NewEngine(KernelOps* kernel) {
    UdsEngine::Transport tr;
    tr.send = [](WifiPacket&) {};  // (no room: nothing is sent in these tests)
    tr.get_mac = [] { return Mac{0x00, 0x1F, 0x32, 0x00, 0x00, 0x01}; };
    return new UdsEngine(kernel, tr);
}

static void RealHubTest() {
    std::printf("plugin/MakeRealDlpHub: the configuration of MakeDlpHub word for word, the adapter as the lobby's network, the probes' identity\n");
    FakeKernel kernel;
    std::unique_ptr<UdsEngine> engine(NewEngine(&kernel));
    const Mac mac = Mac{0x00, 0x1F, 0x32, 0xAB, 0xCD, 0xEF};
    const u64 tid = 0x00040000001D1A00ull;
    RealDlpNet real(engine.get());
    CHECK(engine->lobby_beacon_kind_ == UdsEngine::LobbyBeaconKind::RealDlp);  // (the adapter's constructor: the game is of that mode)
    std::unique_ptr<DlpHub> hub = MakeRealDlpHub(engine.get(), &kernel, tid, mac, &real);
    std::unique_ptr<DlpHub> lobby = MakeDlpHub(engine.get(), &kernel, tid, mac);
    CHECK(hub != nullptr && lobby != nullptr);
    if (hub == nullptr || lobby == nullptr) {
        return;
    }
    CHECK(hub->lobby_.net() == static_cast<LobbyNet*>(&real));  // the lobby runs on the adapter of the real Download Play ...
    CHECK(hub->lobby_.net() != &hub->net_);                     // ... not on the hub's own one, which stays idle
    CHECK(!hub->net_.engine_held());
    CHECK(lobby->lobby_.net() == &lobby->net_);
    // the same configuration as MakeDlpHub's (the game's tid, the hash of the MAC, "3DS Link", the plugin's resolver)
    CHECK_EQ(hub->lobby_.tid_low_, lobby->lobby_.tid_low_);
    CHECK_EQ(hub->lobby_.tid_low_, 0x001D1A00u);
    CHECK_EQ(hub->lobby_.id_.seed, lobby->lobby_.id_.seed);
    CHECK_EQ(hub->lobby_.id_.seed, Fnv1a64(mac.data(), mac.size()));
    for (std::size_t i = 0; i < kNameUnits; ++i) {
        CHECK_EQ(hub->lobby_.id_.name[i], lobby->lobby_.id_.name[i]);
    }
    CHECK(hub->resolver_.map == &MapGameAddress);
    CHECK(hub->resolver_.ctx == nullptr);
    CHECK_EQ(hub->lobby_.max_players(), 4u);  // Luigi's Mansion: four, as its row
    CHECK_EQ(lobby->lobby_.max_players(), 4u);
    CHECK_EQ(real.players(), 4u);             // ... and the adapter is told the same four (its table holds RealDlpNet::kRealPlayers, eight)
    CHECK_EQ(RealDlpNet::kRealPlayers, 8u);
    CHECK(!engine->admission_to_members());   // Luigi's Mansion: its own network broadcasts the Logoff of an admission, as ever
    {
        // Mario Kart 7: the same function gives the lobby AND the adapter the eight of its row (an adapter nobody told has four)
        std::unique_ptr<UdsEngine> engine8(NewEngine(&kernel));
        RealDlpNet real8(engine8.get());
        CHECK_EQ(real8.players(), 4u);
        CHECK(!engine8->admission_to_members());  // (an engine nobody told: off)
        std::unique_ptr<DlpHub> hub8 = MakeRealDlpHub(engine8.get(), &kernel, 0x0004000000030700ull, mac, &real8);
        CHECK(hub8 != nullptr);
        if (hub8 != nullptr) {
            CHECK_EQ(hub8->lobby_.max_players(), 8u);
            CHECK_EQ(real8.players(), 8u);
            CHECK(engine8->admission_to_members());  // Mario Kart 7: the game's own network admits to its members one by one
            CHECK(hub8->lobby_.net() == static_cast<LobbyNet*>(&real8));
            CHECK_EQ(hub8->lobby_.tid_low_, 0x00030700u);
            // ... and a game that asks for seven clients gets seven from both: the lobby announces eight nodes, the adapter agrees to host them
            CHECK_EQ(hub8->lobby_.ServerInitialize(7, 0, nullptr), 0u);
            CHECK_EQ(hub8->lobby_.max_clients_, 7u);
            CHECK_EQ(hub8->lobby_.ServerFinalize(), 0u);
        }
    }
    {
        // Mario Party: Island Tour (the console hosts): its row says its mode and nothing else, so the lobby and the adapter get the default of
        // four, the seed is the hash of the MAC, the game's own network admits as ever; and the 3 clients its Initialize asks for are 3
        std::unique_ptr<UdsEngine> engine_i(NewEngine(&kernel));
        RealDlpNet real_i(engine_i.get());
        std::unique_ptr<DlpHub> hub_i = MakeRealDlpHub(engine_i.get(), &kernel, 0x00040000000F8200ull, mac, &real_i);
        CHECK(hub_i != nullptr);
        if (hub_i != nullptr) {
            CHECK_EQ(hub_i->lobby_.max_players(), 4u);  // Mario Party: Island Tour: four, the default of its row
            CHECK_EQ(real_i.players(), 4u);             // Mario Party: Island Tour: the adapter is told the same four
            CHECK(!engine_i->admission_to_members());   // Mario Party: Island Tour: its own network broadcasts the Logoff of an admission
            CHECK(hub_i->lobby_.net() == static_cast<LobbyNet*>(&real_i));
            CHECK_EQ(hub_i->lobby_.tid_low_, 0x000F8200u);
            CHECK_EQ(hub_i->lobby_.id_.seed, Fnv1a64(mac.data(), mac.size()));  // Mario Party: Island Tour: the hash of the MAC
            CHECK_EQ(hub_i->lobby_.ServerInitialize(3, 0, nullptr), 0u);
            CHECK_EQ(hub_i->lobby_.max_clients_, 3u);  // Mario Party: Island Tour: the three clients the game asks for
            CHECK_EQ(hub_i->lobby_.ServerFinalize(), 0u);
        }
    }
    // ... and the game's other regions (2026-11-14): the same function, the same values, and the low word of THEIR OWN title in the lobby - it is what the child
    // the host announces is made of (00040001 above it: Tests/dlp_real_island_test.cpp, part R), so nothing of the European id is left in an American console's lobby
    for (const u64 region : {0x00040000000F8100ull, 0x00040000000DD400ull, 0x0004000000119300ull, 0x0004000000119B00ull}) {
        std::unique_ptr<UdsEngine> engine_r(NewEngine(&kernel));
        RealDlpNet real_r(engine_r.get());
        std::unique_ptr<DlpHub> hub_r = MakeRealDlpHub(engine_r.get(), &kernel, region, mac, &real_r);
        CHECK(hub_r != nullptr);
        if (hub_r != nullptr) {
            CHECK_EQ(hub_r->lobby_.tid_low_, static_cast<u32>(region));  // a region of Mario Party: Island Tour: the lobby has the low word of its own title
            CHECK(hub_r->lobby_.tid_low_ != 0x000F8200u);
            CHECK_EQ(hub_r->lobby_.max_players(), 4u);
            CHECK_EQ(real_r.players(), 4u);
            CHECK(!engine_r->admission_to_members());
            CHECK(hub_r->lobby_.net() == static_cast<LobbyNet*>(&real_r));
            CHECK_EQ(hub_r->lobby_.id_.seed, Fnv1a64(mac.data(), mac.size()));
            CHECK_EQ(hub_r->lobby_.ServerInitialize(3, 0, nullptr), 0u);
            CHECK_EQ(hub_r->lobby_.max_clients_, 3u);
            CHECK_EQ(hub_r->lobby_.ServerFinalize(), 0u);
        }
    }
    // the probes are the lobby's own identity (a spectator's NodeInfo: nobody shows it, but it is not zero)
    CHECK_EQ(real.me_.seed, Fnv1a64(mac.data(), mac.size()));
    for (std::size_t i = 0; i < kNameUnits; ++i) {
        CHECK_EQ(real.me_.name[i], lobby->lobby_.id_.name[i]);
    }
    // nothing is held, the hub's hints say so, and the adapter is the one asked
    CHECK(!hub->EngineHeld());
    CHECK(!real.Holds());
    CHECK(!engine->initialized());
}

/// A stand-in of the plugin with the members the block of EarlyInit uses, under the same names; the block itself is plugin.cpp's text.
struct FakeHle {
    void SetDlpHub(DlpHub* hub) {
        hub_ = hub;
        ++sets_;
    }
    void SetDlpClnt(bool on) {  // (UdsHle's: only with a hub)
        clnt_ = on && hub_ != nullptr;
        ++clnt_sets_;
    }
    DlpHub* hub_ = nullptr;
    int sets_ = 0;
    bool clnt_ = false;
    int clnt_sets_ = 0;
};
struct FakePlugin {
    FakePlugin(u64 tid, bool active) : engine_(NewEngine(&kernel_)), hle_(new FakeHle()), game_id_(tid), active_(active) {}
    void Log(const char* format, ...) {
        char line[512];
        va_list ap;
        va_start(ap, format);
        std::vsnprintf(line, sizeof(line), format, ap);
        va_end(ap);
        log_.push_back(line);
    }
    void PostNotification(const std::string& text) {
        notices_.push_back(text);
    }
    void Wire() {
#include "plugin_dlp_wiring.inc"
    }
    FakeKernel kernel_;
    std::unique_ptr<UdsEngine> engine_;
    std::unique_ptr<FakeHle> hle_;
    u64 game_id_;
    bool active_;
    Mac console_mac_ = Mac{0x00, 0x1F, 0x32, 0x12, 0x34, 0x56};
    std::unique_ptr<DlpClntBook> dlp_book_;
    std::unique_ptr<RealDlpNet> dlp_real_;
    std::unique_ptr<DlpHub> dlp_hub_;
    bool clnt_room_ = false;
    std::vector<std::string> log_;
    std::vector<std::string> notices_;
};

/// The same stand-in, with the plugin's DlpModeOf hidden by a member that says Lobby for every title: the Lobby branch of EarlyInit (the hub of the
/// plugin's dialect) is run whatever the rows of the table say (L4: so that it stays run once no row is Lobby).
struct FakeLobbyPlugin : FakePlugin {
    using FakePlugin::FakePlugin;
    DlpGameMode DlpModeOf(u64) const { return DlpGameMode::Lobby; }
    void Wire() {
#include "plugin_dlp_wiring.inc"
    }
};

static void ClntLocalTest() {
    std::printf("plugin/ClntStaysLocal (1.1.4, G1, K5): the Download Play application keeps the system's own Download Play unless the settings were read at\n"
                "  the start AND the plugin joins a room by itself; no other title ever stays out for it\n");
    u32 local = 0, room = 0;
    for (const u64 t : kDownloadPlayTitles) {
        for (int k = 0; k < 16; ++k) {
            const bool early = (k & 1) != 0, host = (k & 2) != 0, autoconnect = (k & 4) != 0, menu_off = (k & 8) != 0;
            const bool joins_by_itself = host && (autoconnect || menu_off);  // (Sources/main.cpp's g_want_connected)
            const bool stays = ClntStaysLocal(t, early, host, autoconnect, menu_off);
            CHECK_EQ(stays, !early || !joins_by_itself);
            local += stays ? 1u : 0u;
            room += stays ? 0u : 1u;
        }
        CHECK(ClntStaysLocal(t, false, true, true, true));   // the settings not read at the start: out of it, whatever they say later
        CHECK(!ClntStaysLocal(t, true, true, true, false));  // read, an address, autoconnect: the room
        CHECK(ClntStaysLocal(t, true, false, true, true));   // no address: local
        CHECK(ClntStaysLocal(t, true, true, false, false));  // an address but no autoconnect and the menu on: the plugin does not join by itself
    }
    CHECK_EQ(local, 6u * 13u);
    CHECK_EQ(room, 6u * 3u);
    for (const DlpGame& game : kDlpGames) {
        for (int k = 0; k < 16; ++k) {
            CHECK(!ClntStaysLocal(game.title_id, (k & 1) != 0, (k & 2) != 0, (k & 4) != 0, (k & 8) != 0));
        }
    }
    for (const u64 t : {0x0000000000000000ull, 0x0004000000164800ull, 0x0004001000022000ull, 0x0004000000022100ull, 0x000400000F83D100ull}) {
        CHECK(!ClntStaysLocal(t, false, false, false, false));
    }
}

static void ClntHubTest() {
    std::printf("plugin/MakeClntHub (1.1.4, G1): any game's lobby, eight players, the searcher as the network, the book fed and given the children\n");
    FakeKernel kernel;
    std::unique_ptr<UdsEngine> engine(NewEngine(&kernel));
    const Mac mac = Mac{0x00, 0x1F, 0x32, 0xAB, 0xCD, 0xEF};
    RealDlpNet real(engine.get());
    DlpClntBook book;
    std::unique_ptr<DlpHub> hub = MakeClntHub(engine.get(), &kernel, 0x0004001000022100ull, mac, &real, &book);
    CHECK(hub != nullptr);
    if (hub == nullptr) {
        return;
    }
    CHECK(hub->lobby_.net() == static_cast<LobbyNet*>(&real));
    CHECK(real.ClntBook() == &book);
    CHECK_EQ(hub->lobby_.tid_low_, DlpLobby::kAnyTitleLow);
    CHECK_EQ(hub->lobby_.max_players(), 8u);
    CHECK_EQ(real.players(), 8u);
    CHECK(!engine->admission_to_members());
    CHECK(hub->resolver_.map == &MapGameAddress && hub->resolver_.ctx == nullptr);
    CHECK_EQ(hub->lobby_.id_.seed, Fnv1a64(mac.data(), mac.size()));
    const char kName[] = "3DS Link";
    for (std::size_t i = 0; i < kNameUnits; ++i) {
        CHECK_EQ(hub->lobby_.id_.name[i], i < sizeof(kName) - 1 ? static_cast<u16>(kName[i]) : 0u);
        CHECK_EQ(real.me_.name[i], hub->lobby_.id_.name[i]);
    }
    CHECK_EQ(real.me_.seed, hub->lobby_.id_.seed);
    // the table the plugin's thread publishes on the adapter is the one the book reads (K3), at once
    CHECK(book.children() == nullptr);
    dlp_children::Table table;
    dlp_children::Clear(&table);
    real.SetChildren(&table);
    CHECK(book.children() == &table);
    CHECK(real.children() == &table);
    // a game's hub has no book: dlp:CLNT is answered as an id the service does not have there
    RealDlpNet game_real(engine.get());
    std::unique_ptr<DlpHub> game_hub = MakeRealDlpHub(engine.get(), &kernel, 0x00040000000F8200ull, mac, &game_real);
    CHECK(game_hub != nullptr && game_real.ClntBook() == nullptr);
    std::unique_ptr<DlpHub> lobby_hub = MakeDlpHub(engine.get(), &kernel, 0x000400000010C000ull, mac);
    CHECK(lobby_hub != nullptr && lobby_hub->lobby_.net()->ClntBook() == nullptr);
    CHECK(game_hub != nullptr && game_hub->lobby_.tid_low_ == 0x000F8200u);  // (the games keep their own title's low word)
    CHECK(!real.child_wait_on());  // H5: the Download Play application hosts nothing: its searcher never waits
}

/// H5 (1.1.4): MakeRealDlpHub turns the host's wait for its guests' children on for a game that hosts the real Download Play (its row says RealDlp:
/// Mario Party: Island Tour, Mario Kart 7, Luigi's Mansion, the D6 rows of d6_real, every region) and for no other id the function may be given - a game of the lobby, the
/// probe, a title with no column, the children, the Download Play application.
static void ChildWaitTest() {
    std::printf("plugin/MakeRealDlpHub (1.1.4, H5): the wait for the children for the games that host the real Download Play, and only for them\n");
    FakeKernel kernel;
    const Mac mac = Mac{0x00, 0x1F, 0x32, 0xAB, 0xCD, 0xEF};
    u32 on = 0, off = 0;
    for (const DlpGame& g : kDlpGames) {
        std::unique_ptr<UdsEngine> engine(NewEngine(&kernel));
        RealDlpNet real(engine.get());
        std::unique_ptr<DlpHub> hub = MakeRealDlpHub(engine.get(), &kernel, g.title_id, mac, &real);
        CHECK(hub != nullptr);
        CHECK_EQ(real.child_wait_on(), g.mode == DlpGameMode::RealDlp);
        on += real.child_wait_on() ? 1u : 0u;
        off += real.child_wait_on() ? 0u : 1u;
    }
    CHECK_EQ(on, 16u);   // (the RealDlp ids: Island Tour 5, Mario Kart 7 5, Luigi's Mansion 4, the D6 rows of d6_real)
    CHECK_EQ(off, 14u);
    for (const u64 other : {0x0004000000164800ull, 0x00040001000F8200ull, 0x0004000E000F8200ull, 0x0004001000022100ull, 0x0004000000000000ull,
                            0x000400000F8F8200ull}) {
        std::unique_ptr<UdsEngine> engine(NewEngine(&kernel));
        RealDlpNet real(engine.get());
        std::unique_ptr<DlpHub> hub = MakeRealDlpHub(engine.get(), &kernel, other, mac, &real);
        CHECK(!real.child_wait_on());
    }
    // the adapter answers the lobby with the wait's mask: nothing before a final, whatever the switch
    std::unique_ptr<UdsEngine> engine(NewEngine(&kernel));
    RealDlpNet real(engine.get());
    std::unique_ptr<DlpHub> hub = MakeRealDlpHub(engine.get(), &kernel, 0x00040000000F8200ull, mac, &real);
    CHECK(real.child_wait_on());
    CHECK_EQ(static_cast<const LobbyNet&>(real).ChildrenAwaited(), 0u);
    CHECK(hub != nullptr && hub->lobby_.net()->ChildrenAwaited() == 0u);
}

/// 1.1.4, route B (spec 8 D-B6): the members a host of the real Download Play waits for at its final (OnRoomInfo's filter, AwaitsChildOf): the
/// six ids of the system's Download Play application and the 3DS Link app's Download Play page (kAppTid), and no other id - not 0 (the app on
/// another page), not a game, its child, its prepared title or an update, not a neighbour of kAppTid nor kAppTid under another high word.
static void AwaitsChildTest() {
    std::printf("plugin/OnRoomInfo (1.1.4, route B): a host waits for a guest in the Download Play application or in 3DS Link's Download Play page\n");
    u32 yes = 0;
    for (const u64 t : kDownloadPlayTitles) {
        CHECK(AwaitsChildOf(t));
        yes += AwaitsChildOf(t) ? 1u : 0u;
    }
    CHECK(AwaitsChildOf(title_rules::kAppTid));
    CHECK(AwaitsChildOf(0x000400000F83D100ull));  // (the id written out)
    CHECK_EQ(yes, 6u);
    const u64 app = title_rules::kAppTid;
    const u64 others[] = {0x0000000000000000ull, app - 1, app + 1, app | 0x0000000100000000ull, app ^ 0x0000001000000000ull, 0x0004000E0F83D100ull,
                        0x000400000F83D000ull, 0x000400000F83D200ull, 0x000400000F8F8200ull, 0x00040000000F8200ull, 0x00040001000F8200ull,
                        0x0004000E000F8200ull, 0x0004001000022000ull, 0x0004001000022200ull, 0xFFFFFFFFFFFFFFFFull};
    for (const u64 t : others) {
        CHECK(!AwaitsChildOf(t));
    }
    u32 rows = 0;
    for (const DlpGame& g : kDlpGames) {
        CHECK(!AwaitsChildOf(g.title_id));
        CHECK(!AwaitsChildOf(g.title_id | 0x0000000100000000ull));
        CHECK(!AwaitsChildOf(title_rules::PreparedOf(g.title_id)));
        ++rows;
    }
    CHECK(rows > 0);
}

static void LobbyStandInTest() {
    std::printf("plugin/EarlyInit's Lobby branch, with a stand-in that says Lobby: the hub of the dialect, on its own adapter, four players\n");
    for (const bool active : {true, false}) {
        FakeLobbyPlugin p(0x000400000010C000ull, active);
        p.Wire();
        CHECK_EQ(p.dlp_hub_ != nullptr, active);
        CHECK_EQ(p.hle_->sets_, active ? 1 : 0);
        CHECK(p.hle_->hub_ == p.dlp_hub_.get());
        CHECK(p.dlp_real_ == nullptr);  // (the dialect: no adapter of the real Download Play)
        CHECK(p.engine_->lobby_beacon_kind_ == UdsEngine::LobbyBeaconKind::Ours);
        CHECK(!p.engine_->admission_to_members());
        CHECK_EQ(p.log_.size(), active ? 1u : 0u);
        if (!active || p.dlp_hub_ == nullptr) {
            continue;
        }
        DlpHub& hub = *p.dlp_hub_;
        CHECK(hub.lobby_.net() == &hub.net_);
        CHECK(p.log_[0].find("the Download Play lobby is carried by the room") != std::string::npos);
        CHECK(p.log_[0].find("title id 00040000") != std::string::npos);  // (the low word is printed with %lx: 32 bits on the console, 64 here)
        CHECK_EQ(hub.lobby_.tid_low_, 0x0010C000u);
        CHECK_EQ(hub.lobby_.id_.seed, Fnv1a64(p.console_mac_.data(), p.console_mac_.size()));
        CHECK_EQ(hub.lobby_.max_players(), 4u);
        CHECK_EQ(hub.lobby_.ServerInitialize(7, 0, nullptr), 0u);
        CHECK_EQ(hub.lobby_.max_clients_, 3u);
        CHECK_EQ(hub.lobby_.ServerFinalize(), 0u);
        CHECK(!hub.EngineHeld());
    }
}

static void WiringTest() {
    std::printf("plugin/EarlyInit's decision, for every row, other games and an inactive plugin: only RealDlp gets the real adapter\n");
    std::vector<u64> titles;
    for (const DlpGame& game : kDlpGames) {
        titles.push_back(game.title_id);
    }
    // games outside the table (Tri Force Heroes, Star Rush, Fates, Pokemon, a kiosk demo of Kirby Triple Deluxe, the empty id, ...), neighbours of the table's ids,
    // a row of title_rules with no Download Play column (Ridge Racer 3D) and two Download Play children (Island Tour's, Mario Kart 7's): no hub
    for (const u64 t : {0x0000000000000000ull, 0x0004000000164800ull, 0x0004000000199300ull, 0x000400000017E600ull, 0x0004000000055D00ull,
                        0x0004000000122C00ull, 0x00040000001D1B00ull, 0x00040000001D1A01ull, 0x00040002001D1A00ull, 0x000400000010C100ull,
                        0xFFFFFFFFFFFFFFFFull, 0x0004000000033B00ull, 0x00040001000F8200ull, 0x0004000100030700ull}) {
        titles.push_back(t);
    }
    // 1.1.4 (G1): the six ids of the system's Download Play application (SystemClient: the book, the searcher and the hub of dlp:CLNT), and two
    // neighbours that are not it
    for (const u64 t : kDownloadPlayTitles) {
        titles.push_back(t);
    }
    titles.push_back(0x0004001000022000ull);
    titles.push_back(0x0004000000022100ull);
    // 1.1.4 (G4): the prepared child of every id of every row (title_rules::PreparedOf) and 3DS Link: no hub, no adapter - the prepared child is a
    // client of its game's own network, and the Download Play child never travels through the room
    for (std::size_t r = 0; r < title_rules::kRowCount; ++r) {
        for (const u32 low : title_rules::kRows[r].low) {
            if (low != 0) {
                titles.push_back(title_rules::PreparedOf(0x0004000000000000ull | low));
            }
        }
    }
    titles.push_back(title_rules::kAppTid);
    u32 lobby_hubs = 0, real_hubs = 0, none = 0, eight_hubs = 0, members_engines = 0, island_hubs = 0, island_region_hubs = 0, clnt_hubs = 0;
    for (const u64 tid : titles) {
        for (const bool active : {true, false}) {
            FakePlugin p(tid, active);
            p.Wire();
            const DlpGameMode mode = DlpModeOf(tid);
            const bool system_client = mode == DlpGameMode::SystemClient;
            const bool want_hub = active && (mode == DlpGameMode::Lobby || mode == DlpGameMode::RealDlp || system_client);
            CHECK_EQ(p.dlp_hub_ != nullptr, want_hub);
            CHECK_EQ(p.hle_->sets_, want_hub ? 1 : 0);
            CHECK(p.hle_->hub_ == p.dlp_hub_.get());
            CHECK_EQ(p.log_.size(), want_hub ? 1u : 0u);
            // dlp:CLNT, its book and its notice: the Download Play application with the plugin active, nothing else
            CHECK_EQ(p.hle_->clnt_, active && system_client);
            CHECK_EQ(p.hle_->clnt_sets_, active && system_client ? 1 : 0);
            CHECK_EQ(p.clnt_room_, active && system_client);
            CHECK_EQ(p.dlp_book_ != nullptr, active && system_client);
            CHECK_EQ(p.notices_.size(), active && system_client ? 1u : 0u);
            // the adapter of the real Download Play exists for a RealDlp game and for the Download Play application (its constructor is the only
            // thing that turns the engine's kind)
            const bool want_real = active && (mode == DlpGameMode::RealDlp || system_client);
            CHECK_EQ(p.dlp_real_ != nullptr, want_real);
            CHECK(p.engine_->lobby_beacon_kind_ == (want_real ? UdsEngine::LobbyBeaconKind::RealDlp : UdsEngine::LobbyBeaconKind::Ours));
            CHECK(!p.engine_->initialized());
            // the engine's switch of the title (the Logoff of an admission to the members of the game's own network one by one): on for the
            // five Mario Kart 7 ids with the plugin active, off for every other row, every other game and an inactive plugin
            CHECK_EQ(p.engine_->admission_to_members(),
                     active && (tid == 0x0004000000030700ull || tid == 0x0004000000030800ull || tid == 0x0004000000030600ull ||
                                tid == 0x0004000000030A00ull || tid == 0x000400000008B400ull));
            members_engines += p.engine_->admission_to_members() ? 1u : 0u;
            if (!want_hub) {
                ++none;
                continue;
            }
            DlpHub& hub = *p.dlp_hub_;
            if (system_client) {
                // the hub of dlp:CLNT: any game's lobby (the lobby's low word is the "any title" mark, not the application's), the tables' eight,
                // the searcher of the real Download Play as its network, which feeds the book; the hash of the MAC; nothing of the creator
                ++clnt_hubs;
                CHECK_EQ(hub.lobby_.tid_low_, DlpLobby::kAnyTitleLow);
                CHECK(hub.lobby_.tid_low_ != static_cast<u32>(tid));
                CHECK_EQ(hub.lobby_.max_players(), 8u);
                CHECK_EQ(p.dlp_real_->players(), 8u);
                CHECK(hub.lobby_.net() == static_cast<LobbyNet*>(p.dlp_real_.get()));
                CHECK(p.dlp_real_->ClntBook() == p.dlp_book_.get());
                CHECK(hub.lobby_.net()->ClntBook() == p.dlp_book_.get());
                CHECK_EQ(hub.lobby_.id_.seed, Fnv1a64(p.console_mac_.data(), p.console_mac_.size()));
                CHECK_EQ(p.dlp_real_->me_.seed, hub.lobby_.id_.seed);
                CHECK(p.dlp_book_->children() == nullptr);  // (no table until LateInit reads the index)
                CHECK(!hub.EngineHeld());
                CHECK(p.log_[0].find("the Download Play application: dlp:CLNT is served by the plugin") != std::string::npos);
                CHECK(p.log_[0].find("00:1F") == std::string::npos && p.log_[0].find("00:1f") == std::string::npos);  // (no MAC in the line)
                CHECK(p.notices_[0].find("3DS Link") != std::string::npos);
                continue;
            }
            CHECK_EQ(hub.lobby_.tid_low_, static_cast<u32>(tid));
            CHECK_EQ(hub.lobby_.id_.seed, Fnv1a64(p.console_mac_.data(), p.console_mac_.size()));
            CHECK(!hub.EngineHeld());
            // the ceiling of players the plugin's own decision gives the lobby: eight for the five Mario Kart 7 ids (written by hand), four for
            // every other row; and what a game gets when it asks for 0, 1, 3, 4, 7, 8, 15 or 255 clients (the network is not touched: no lobby is open)
            const bool is_mk7 = tid == 0x0004000000030700ull || tid == 0x0004000000030800ull || tid == 0x0004000000030600ull ||
                                tid == 0x0004000000030A00ull || tid == 0x000400000008B400ull;
            const u32 ceiling = is_mk7 ? 8u : 4u;
            CHECK_EQ(hub.lobby_.max_players(), ceiling);
            eight_hubs += hub.lobby_.max_players() == 8 ? 1u : 0u;
            for (const u32 asked : {0u, 1u, 3u, 4u, 7u, 8u, 15u, 255u}) {
                CHECK_EQ(hub.lobby_.ServerInitialize(asked, 0, nullptr), 0u);
                CHECK_EQ(hub.lobby_.max_clients_, asked < 1 ? 1u : (asked > ceiling - 1 ? ceiling - 1 : asked));
            }
            CHECK_EQ(hub.lobby_.ServerFinalize(), 0u);
            CHECK(!hub.EngineHeld());
            if (want_real) {
                ++real_hubs;
                island_hubs += tid == 0x00040000000F8200ull ? 1u : 0u;
                island_region_hubs += (tid == 0x00040000000F8100ull || tid == 0x00040000000DD400ull || tid == 0x0004000000119300ull || tid == 0x0004000000119B00ull) ? 1u : 0u;
                CHECK(hub.lobby_.net() == static_cast<LobbyNet*>(p.dlp_real_.get()));
                CHECK_EQ(p.dlp_real_->players(), ceiling);  // the adapter has the ceiling of the row, as the lobby: 4 for Luigi's Mansion, 8 for Mario Kart 7
                CHECK(p.log_[0].find("the real Download Play is carried by the room") != std::string::npos);
                CHECK_EQ(p.dlp_real_->me_.seed, hub.lobby_.id_.seed);
            } else {
                ++lobby_hubs;
                CHECK(hub.lobby_.net() == &hub.net_);  // the hub's own adapter, as before 1.1.3
                CHECK(p.log_[0].find("the Download Play lobby is carried by the room") != std::string::npos);
            }
            CHECK(p.log_[0].find("title id ") != std::string::npos);
        }
    }
    CHECK_EQ(real_hubs, 16u);  // the RealDlp ids: Luigi's Mansion 4, Mario Kart 7 5, Mario Party: Island Tour 5, and the rows of D6 (d6_real above)
    CHECK_EQ(island_hubs, 1u);  // Mario Party: Island Tour with the plugin active: ONE hub, on the adapter of the real Download Play
    CHECK_EQ(island_region_hubs, 4u);  // ... and one for each of its other four regions (USA, Japan, Taiwan, Korea), on the same adapter
    CHECK_EQ(lobby_hubs, 10u);  // the Lobby ids: the rows of D6 that are not in d6_real
    CHECK_EQ(eight_hubs, 5u);  // Mario Kart 7 and nobody else plays in eight
    CHECK_EQ(members_engines, 5u);  // ... and nobody else has the admissions of its own network sent to the members one by one
    CHECK_EQ(clnt_hubs, 6u);  // the six ids of the system's Download Play application (1.1.4, G1), and only with the plugin active
    CHECK_EQ(none, static_cast<u32>(titles.size() * 2 - 32));  // (32 ids with a hub: the Lobby and the RealDlp ones, 26, + 6 of the Download Play application)
}

// ---- 1.1.4 (G4, contract K8): the prepared child ------------------------------------------------------------------------------------------------
// Every sold title of the catalogue (Tests/title_families.inc): {low word, family}.
struct SoldTitle {
    u32 low;
    u32 family;
};
const SoldTitle kSold[] = {
#include "title_families.inc"
};

/// An index as the app writes it (dlp_children::FormatRow), with the rows given.
static std::string IndexText(std::initializer_list<dlp_children::Row> rows) {
    std::string text = std::string(dlp_children::kIndexHeader) + "\n";
    for (const dlp_children::Row& r : rows) {
        char line[dlp_children::kRowBytes + 1];
        CHECK(dlp_children::FormatRow(r, line) > 0);
        text += line;
    }
    return text;
}

static bool SameRow(const dlp_children::Row& a, const dlp_children::Row& b) {
    return a.child == b.child && a.game == b.game && a.prepared == b.prepared && a.version == b.version && a.where == b.where;
}

static void PreparedChildTest() {
    std::printf("plugin/the prepared child (1.1.4, G4, K8): a title of the band with its row of the index, and nothing else; the early join; the loader's words\n");
    dlp_children::Index scratch;
    const u64 game = 0x00040000000F8200ull;
    const u64 child = 0x00040001000F8200ull;
    const u64 prepared = title_rules::PreparedOf(game);
    CHECK_EQ(prepared, 0x000400000F8F8200ull);
    const dlp_children::Row island = {child, game, prepared, 1024, 'S'};
    const dlp_children::Row mk7 = {0x0004000100030700ull, 0x0004000000030700ull, title_rules::PreparedOf(0x0004000000030700ull), 3, 'C'};
    const std::string both = IndexText({mk7, island});
    // with its row: the child and the game of that row, whose rules and name it takes - never its Download Play column (the child never travels
    // through the room: no hub, no adapter, as WiringTest holds for every prepared id); the process's own id has no rules of its own
    dlp_children::Row row = {};
    CHECK(PreparedChildRow(prepared, both.data(), both.size(), &scratch, &row));
    CHECK(SameRow(row, island));
    CHECK_EQ(title_rules::RulesFor(row.game).cap_drop_max, 2048u);
    CHECK_EQ(title_rules::RulesFor(row.game).same_frame_ms, 30u);
    CHECK_EQ(title_rules::RulesFor(prepared).cap_drop_max, 0u);  // (why the plugin keeps rules_id_: the process's id is in no row)
    CHECK(std::string(title_rules::NameOf(row.game)) == "Mario Party: Island Tour");
    CHECK(std::string(title_rules::NameOf(prepared)).empty());
    CHECK(DlpModeOf(prepared) == DlpGameMode::None);
    CHECK(!title_rules::AppRulesFor(prepared).worker_off_core1 && !title_rules::AppRulesFor(prepared).late_soc_block &&
          !title_rules::AppRulesFor(prepared).soc_guest);  // (the start-up rules are the process's: as any title)
    const u64 mk7_prepared = mk7.prepared;
    CHECK(PreparedChildRow(mk7_prepared, both.data(), both.size(), &scratch, &row));
    CHECK(SameRow(row, mk7));
    // without its row: no, and the row given is untouched
    const dlp_children::Row untouched = {0x0004000100000100ull, 0x0004000000000100ull, 0, 7, 'S'};
    const std::string only_mk7 = IndexText({mk7});
    const std::string empty_index = IndexText({});
    std::string other_version = both;
    other_version.replace(0, std::strlen(dlp_children::kIndexHeader), "#dlp-children 2");
    std::string broken = both;  // (the island row names another prepared title: a line the parser refuses)
    const std::size_t at = broken.find("000400000F8F8200");
    CHECK(at != std::string::npos);
    broken.replace(at, 16, "000400000F8F8300");
    const std::string cut = both.substr(0, both.size() - 1);  // (the last line - the island's - without its '\n')
    const std::string no_header = both.substr(both.find('\n') + 1);
    struct Case {
        const char* what;
        const char* text;
        std::size_t size;
    } cases[] = {
        {"another child's row only", only_mk7.data(), only_mk7.size()},
        {"an index with no row", empty_index.data(), empty_index.size()},
        {"no index", nullptr, 0},
        {"an empty file", "", 0},
        {"another version", other_version.data(), other_version.size()},
        {"a line refused", broken.data(), broken.size()},
        {"its line cut", cut.data(), cut.size()},
        {"no header", no_header.data(), no_header.size()},
    };
    for (const Case& c : cases) {
        row = untouched;
        const bool taken = PreparedChildRow(prepared, c.text, c.size, &scratch, &row);
        CHECK(!taken);
        CHECK(SameRow(row, untouched));
        if (taken) {
            std::printf("  taken with %s\n", c.what);
        }
    }
    row = untouched;
    CHECK(!PreparedChildRow(prepared, both.data(), both.size(), nullptr, &row));  // (no scratch: no)
    CHECK(SameRow(row, untouched));
    // 3DS Link and a homebrew of the band, the child, the game, an update, a prepared id put in another high word: never, with the index there.
    // The build 2h: the system id 00040010|low IS the child (the one id of the form made now) - 00040001 (M3c1's), 0004000E, 00040012, 00040002 never,
    // nor the system's Download Play application, nor a system id out of the band or of a homebrew of the band
    const u64 never[] = {title_rules::kAppTid, 0x000400000F8F8300ull, 0x000400000F800000ull, child, game, 0x0004000E000F8200ull, 0x000400010F8F8200ull,
                        0x0004000E0F8F8200ull, 0x000400120F8F8200ull, 0x000400020F8F8200ull, 0x0004001000022100ull, 0x000400100F7FFF00ull,
                        0x0004001010000000ull, 0x000400100F83D100ull, 0x000400100F8F8300ull, 0x0000000000000000ull};
    for (const u64 t : never) {
        CHECK(!PreparedChildRow(t, both.data(), both.size(), &scratch, &row));
    }
    row = untouched;
    CHECK(PreparedChildRow(0x000400100F8F8200ull, both.data(), both.size(), &scratch, &row) && row.prepared == prepared && row.game == game);
    // the whole sold range: a game is never taken for a prepared child - whatever its index says - and its prepared title always is, with its row
    u32 games_taken = 0, games_in_band = 0, prepared_found = 0, system_found = 0;
    for (const SoldTitle& t : kSold) {
        const u64 g = 0x0004000000000000ull | t.low;
        const u64 p = title_rules::PreparedOf(g);
        const dlp_children::Row r = {0x0004000100000000ull | t.low, g, p, 0, 'S'};
        const std::string text = IndexText({r});
        dlp_children::Row got = {};
        games_taken += PreparedChildRow(g, text.data(), text.size(), &scratch, &got) ? 1u : 0u;
        games_in_band += title_rules::IsPreparedId(g) ? 1u : 0u;
        prepared_found += PreparedChildRow(p, text.data(), text.size(), &scratch, &got) && SameRow(got, r) ? 1u : 0u;
        // (the build 2h) the process of its system id is the same child, with the same row; a game under 00040010 never is
        system_found += PreparedChildRow(title_rules::PreparedSystemOf(p), text.data(), text.size(), &scratch, &got) && SameRow(got, r) ? 1u : 0u;
        games_taken += PreparedChildRow((g & 0xFFFFFFFFull) | 0x0004001000000000ull, text.data(), text.size(), &scratch, &got) ? 1u : 0u;
        CHECK(DlpModeOf(title_rules::PreparedSystemOf(p)) == DlpGameMode::None);
        CHECK(DlpModeOf(p) == DlpGameMode::None);  // (and no prepared title has a Download Play column: no hub)
    }
    CHECK_EQ(sizeof(kSold) / sizeof(kSold[0]), 5217u);
    CHECK_EQ(games_taken, 0u);
    CHECK_EQ(games_in_band, 0u);
    CHECK_EQ(prepared_found, 5217u);
    CHECK_EQ(system_found, 5217u);

    // the early join: the prepared child only, the plugin on, a New 3DS, the settings read at the start and saying the plugin joins by itself
    u32 early = 0;
    for (int k = 0; k < 128; ++k) {
        const bool pc = (k & 1) != 0, active = (k & 2) != 0, is_new = (k & 4) != 0, read = (k & 8) != 0, host = (k & 16) != 0,
                   autoconnect = (k & 32) != 0, menu_off = (k & 64) != 0;
        const bool want = pc && active && is_new && read && host && (autoconnect || menu_off);  // (Sources/main.cpp's g_want_connected)
        CHECK_EQ(JoinsEarly(pc, active, is_new, read, host, autoconnect, menu_off), want);
        early += want ? 1u : 0u;
    }
    CHECK_EQ(early, 3u);
    CHECK(!JoinsEarly(false, true, true, true, true, true, true));  // (every other title: never early)
    CHECK(!JoinsEarly(true, true, false, true, true, true, true));   // (an Old 3DS: at the first frame)

    // the line of the child's dlp:SRVR says what the game is told: TRUE only with the plugin on, the hooks in and the service made (G4, review:
    // the plugin off or the hooks out leave the game with the real dlp:SRVR, "not a child", whatever the plugin made)
    u32 says_true = 0;
    for (int k = 0; k < 8; ++k) {
        const bool service = (k & 1) != 0, active = (k & 2) != 0, hooked = (k & 4) != 0;
        const char* const line = ChildSrvrLine(service, active, hooked);
        const bool is_true = std::strstr(line, "IsChild says TRUE") != nullptr;
        CHECK_EQ(is_true, service && active && hooked);
        CHECK(is_true || std::strstr(line, "IsChild says false") != nullptr);
        says_true += is_true ? 1u : 0u;
    }
    CHECK_EQ(says_true, 1u);
    CHECK(std::strstr(ChildSrvrLine(true, false, false), "the plugin is off") != nullptr);  // (the reviewer's case: no memory for the network)
    CHECK(std::strstr(ChildSrvrLine(true, true, false), "no hooks") != nullptr);           // (no gate page, or diag-no-hooks)
    CHECK(std::strstr(ChildSrvrLine(false, true, true), "a hub, or no memory") != nullptr);

    // the loader's words: copied whole, wiped from the header, nothing past the 32 touched; "found" when one is not 0
    volatile u32 header[kLoaderWords + 8];
    u32 shelter[kLoaderWords];
    for (std::size_t i = 0; i < kLoaderWords + 8; ++i) {
        header[i] = i < kLoaderWords ? static_cast<u32>(0x4A4C5A41u + 0x01010101u * i) : 0xCAFEF00Du;
    }
    std::memset(shelter, 0xEE, sizeof(shelter));
    CHECK(TakeLoaderWords(header, shelter));
    u32 copied = 0, wiped = 0, beyond = 0;
    for (std::size_t i = 0; i < kLoaderWords + 8; ++i) {
        if (i < kLoaderWords) {
            copied += shelter[i] == static_cast<u32>(0x4A4C5A41u + 0x01010101u * i) ? 1u : 0u;
            wiped += header[i] == 0 ? 1u : 0u;
        } else {
            beyond += header[i] == 0xCAFEF00Du ? 1u : 0u;
        }
    }
    CHECK_EQ(copied, 32u);
    CHECK_EQ(wiped, 32u);
    CHECK_EQ(beyond, 8u);
    std::memset(shelter, 0xEE, sizeof(shelter));
    CHECK(!TakeLoaderWords(header, shelter));  // (all zero now: none)
    u32 zero = 0;
    for (const u32 w : shelter) {
        zero += w == 0 ? 1u : 0u;
    }
    CHECK_EQ(zero, 32u);
    header[kLoaderWords - 1] = 1;
    CHECK(TakeLoaderWords(header, shelter));  // (the last word alone is "found")
    CHECK_EQ(shelter[kLoaderWords - 1], 1u);
    CHECK_EQ(header[kLoaderWords - 1], 0u);
}

}  // namespace ctr
}  // namespace azl

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

int main() {
    azl::ctr::TableTest();
    azl::ctr::SeedTest();
    azl::ctr::MapTest();
    azl::ctr::HubTest();
    azl::ctr::RealHubTest();
    azl::ctr::WiringTest();
    azl::ctr::LobbyStandInTest();
    azl::ctr::ClntLocalTest();
    azl::ctr::ClntHubTest();
    azl::ctr::ChildWaitTest();
    azl::ctr::AwaitsChildTest();
    azl::ctr::PreparedChildTest();
    std::printf("\n%d checks, %d failures\n", azl::ctr::g_checks, azl::ctr::g_failures);
    return azl::ctr::g_failures == 0 ? 0 : 1;
}
