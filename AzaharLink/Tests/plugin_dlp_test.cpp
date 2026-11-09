// The Download Play wiring of Sources/ctr/plugin.cpp on the PC: the table of title ids and modes (DlpModeOf, AsksDlpOnlyForTheWireless),
// the friend code seed (Fnv1a64), the resolver of the game's buffers (MapGameAddress) and the hub's configuration (MakeDlpHub, and 1.1.3's
// MakeRealDlpHub). plugin.cpp cannot be built on the PC (libctrpf), so this test reads it: plugin_dlp_extract.py cuts the text from
// "struct DlpGame {" to the end of MakeRealDlpHub out of plugin.cpp into plugin_dlp_slice.inc, and this file compiles that text, unchanged, in the
// namespace it lives in. (Mutation round of phase 3, label "mutants": none of these lines was run by any test; Tests/policy_check.sh could only pin
// their text.) 1.1.3, phase F5: the extractor also cuts the block of Plugin::EarlyInit that decides, by the table, which hub a game gets
// (plugin_dlp_wiring.inc), and WiringTest runs it, unchanged, inside a stand-in of the plugin for EVERY row of the table, for games that are not
// in it and with the plugin inactive: only the RealDlp rows (Luigi's Mansion) get the adapter of the real Download Play; every other row and
// every other game gets exactly what it got before (a Lobby hub on the hub's own adapter, or nothing) - R11 of PROGETTO-DLP-VERO-1.1.3.md.
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

namespace azl {
namespace ctr {
namespace {
#include "plugin_dlp_slice.inc"
}  // namespace

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
    std::printf("plugin/the table of title ids: exactly these thirteen, in this order, nothing else (Luigi's Mansion in RealDlp)\n");
    struct Want {
        u64 tid;
        DlpGameMode mode;
        bool console_seed;
    };
    // written out by hand, in the order of kDlpGames (the mode and the seed flag of every title the plugin knows about; the comments of
    // plugin.cpp say which are seen on a console). Whoever adds, moves or removes a row of kDlpGames changes this table in the same commit.
    static const Want want[] = {
        {0x00040000001C2100ull, DlpGameMode::ProbeOnly, false},  // Kirby Battle Royale
        {0x00040000001C2000ull, DlpGameMode::ProbeOnly, false},  // Kirby Battle Royale, another region
        {0x000400000010C000ull, DlpGameMode::Lobby, false},      // Kirby Triple Deluxe
        {0x000400000010BF00ull, DlpGameMode::Lobby, false},      // Kirby Triple Deluxe, another region
        {0x0004000000183600ull, DlpGameMode::Lobby, false},      // Kirby Planet Robobot
        {0x000400000007C800ull, DlpGameMode::Lobby, true},       // Mario Tennis Open, Europe
        {0x000400000007C700ull, DlpGameMode::Lobby, true},       // Mario Tennis Open, USA
        {0x0004000000064D00ull, DlpGameMode::Lobby, true},       // Mario Tennis Open, Japan
        {0x00040000000B9100ull, DlpGameMode::Lobby, true},       // Mario Tennis Open, Taiwan
        {0x00040000001D1A00ull, DlpGameMode::RealDlp, false},    // Luigi's Mansion (2018), Europe (1.1.3: the real Download Play)
        {0x00040000001D1900ull, DlpGameMode::RealDlp, false},    // Luigi's Mansion (2018), USA
        {0x00040000001D1800ull, DlpGameMode::RealDlp, false},    // Luigi's Mansion (2018), Japan
        {0x00040000001D4F00ull, DlpGameMode::RealDlp, false},    // Luigi's Mansion (2018), Korea
    };
    constexpr std::size_t kRows = sizeof(kDlpGames) / sizeof(kDlpGames[0]);
    constexpr std::size_t kWant = sizeof(want) / sizeof(want[0]);
    CHECK_EQ(kRows, kWant);
    // row by row: the same title, the same mode and the same seed flag (the size check alone would not see two rows swapped or one replaced)
    for (std::size_t i = 0; i < kRows && i < kWant; ++i) {
        CHECK_EQ(kDlpGames[i].title_id, want[i].tid);
        CHECK(kDlpGames[i].mode == want[i].mode);
        CHECK_EQ(kDlpGames[i].console_seed, want[i].console_seed);
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
        // the console's own friend code seed only where the row says so (Mario Tennis Open), and only for a game that has a hub
        CHECK_EQ(LobbySeedIsConsoles(w.tid), w.console_seed);
        if (w.console_seed) {
            CHECK(DlpModeOf(w.tid) == DlpGameMode::Lobby);
        }
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
            }
        }
    }
    // exactly the four Mario Tennis Open ids have the console's seed: the game matches its lobby players with their nwm::UDS nodes by it;
    // Kirby and Luigi's Mansion (which do not) keep the hash of the MAC
    std::size_t seeded = 0;
    for (const DlpGame& game : kDlpGames) {
        seeded += game.console_seed ? 1u : 0u;
    }
    CHECK_EQ(seeded, 4u);
    CHECK(!LobbySeedIsConsoles(0));
    // games that must keep the real services (Tri Force Heroes, Mario Party Star Rush, Fire Emblem Fates, Pokemon, the empty id), and
    // Mario Party: Island Tour, out of the table again (it has no Download Play client of its own: nobody can join its lobby)
    static const u64 others[] = {0, 0x0004000000164800ull, 0x0004000000199300ull, 0x000400000017E600ull, 0x0004000000055D00ull, 0x00040000001B5100ull,
                                 0x000400000010C100ull, 0x0004000000183700ull, 0x00040000001C2200ull, 0xFFFFFFFFFFFFFFFFull,
                                 0x00040000000F8200ull, 0x00040000000F8100ull, 0x00040000000DD400ull, 0x0004000000119300ull, 0x0004000000119B00ull};
    for (const u64 t : others) {
        CHECK(DlpModeOf(t) == DlpGameMode::None);
        CHECK(!AsksDlpOnlyForTheWireless(t));
        CHECK(!LobbySeedIsConsoles(t));
    }
    // the hub exists only for Lobby and RealDlp: ProbeOnly and None have none (WiringTest runs the plugin's own decision)
    CHECK(DlpModeOf(0x00040000001C2100ull) != DlpGameMode::Lobby);
    // exactly the four Luigi's Mansion ids are RealDlp, and none of them has the console's seed
    std::size_t real = 0;
    for (const DlpGame& game : kDlpGames) {
        if (game.mode == DlpGameMode::RealDlp) {
            ++real;
            CHECK(!game.console_seed);
            CHECK((game.title_id & 0xFFFFFFFF00000000ull) == 0x0004000000000000ull);
        }
    }
    CHECK_EQ(real, 4u);
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
    DlpHub* hub_ = nullptr;
    int sets_ = 0;
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
    void Wire() {
#include "plugin_dlp_wiring.inc"
    }
    FakeKernel kernel_;
    std::unique_ptr<UdsEngine> engine_;
    std::unique_ptr<FakeHle> hle_;
    u64 game_id_;
    bool active_;
    Mac console_mac_ = Mac{0x00, 0x1F, 0x32, 0x12, 0x34, 0x56};
    std::unique_ptr<RealDlpNet> dlp_real_;
    std::unique_ptr<DlpHub> dlp_hub_;
    std::vector<std::string> log_;
};

static void WiringTest() {
    std::printf("plugin/EarlyInit's decision, for every row, other games and an inactive plugin: only RealDlp gets the real adapter\n");
    std::vector<u64> titles;
    for (const DlpGame& game : kDlpGames) {
        titles.push_back(game.title_id);
    }
    // games outside the table (Tri Force Heroes, Star Rush, Fates, Pokemon, Island Tour, the empty id, ...) and neighbours of the table's ids
    for (const u64 t : {0x0000000000000000ull, 0x0004000000164800ull, 0x0004000000199300ull, 0x000400000017E600ull, 0x0004000000055D00ull,
                        0x00040000000F8200ull, 0x00040000001D1B00ull, 0x00040000001D1A01ull, 0x00040002001D1A00ull, 0x000400000010C100ull,
                        0xFFFFFFFFFFFFFFFFull}) {
        titles.push_back(t);
    }
    u32 lobby_hubs = 0, real_hubs = 0, none = 0;
    for (const u64 tid : titles) {
        for (const bool active : {true, false}) {
            FakePlugin p(tid, active);
            p.Wire();
            const DlpGameMode mode = DlpModeOf(tid);
            const bool want_hub = active && (mode == DlpGameMode::Lobby || mode == DlpGameMode::RealDlp);
            CHECK_EQ(p.dlp_hub_ != nullptr, want_hub);
            CHECK_EQ(p.hle_->sets_, want_hub ? 1 : 0);
            CHECK(p.hle_->hub_ == p.dlp_hub_.get());
            CHECK_EQ(p.log_.size(), want_hub ? 1u : 0u);
            // the adapter of the real Download Play exists for a RealDlp game only (its constructor is the only thing that turns the engine's kind)
            const bool want_real = active && mode == DlpGameMode::RealDlp;
            CHECK_EQ(p.dlp_real_ != nullptr, want_real);
            CHECK(p.engine_->lobby_beacon_kind_ == (want_real ? UdsEngine::LobbyBeaconKind::RealDlp : UdsEngine::LobbyBeaconKind::Ours));
            CHECK(!p.engine_->initialized());
            if (!want_hub) {
                ++none;
                continue;
            }
            DlpHub& hub = *p.dlp_hub_;
            CHECK_EQ(hub.lobby_.tid_low_, static_cast<u32>(tid));
            CHECK_EQ(hub.lobby_.id_.seed, Fnv1a64(p.console_mac_.data(), p.console_mac_.size()));
            CHECK(!hub.EngineHeld());
            if (want_real) {
                ++real_hubs;
                CHECK(hub.lobby_.net() == static_cast<LobbyNet*>(p.dlp_real_.get()));
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
    CHECK_EQ(real_hubs, 4u);   // the four Luigi's Mansion rows
    CHECK_EQ(lobby_hubs, 7u);  // Kirby Triple Deluxe (2), Planet Robobot, Mario Tennis Open (4)
    CHECK_EQ(none, static_cast<u32>(titles.size() * 2 - 11));
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
    std::printf("\n%d checks, %d failures\n", azl::ctr::g_checks, azl::ctr::g_failures);
    return azl::ctr::g_failures == 0 ? 0 : 1;
}
