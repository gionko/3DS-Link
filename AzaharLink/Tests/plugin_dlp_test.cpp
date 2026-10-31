// The Download Play wiring of Sources/ctr/plugin.cpp on the PC: the table of title ids and modes (DlpModeOf, AsksDlpOnlyForTheWireless),
// the friend code seed (Fnv1a64), the resolver of the game's buffers (MapGameAddress) and the hub's configuration (MakeDlpHub).
// plugin.cpp cannot be built on the PC (libctrpf), so this test reads it: plugin_dlp_extract.py cuts the text from "struct DlpGame {" to the end
// of MakeDlpHub out of plugin.cpp into plugin_dlp_slice.inc, and this file compiles that text, unchanged, in the namespace it lives in.
// (Mutation round of phase 3, label "mutants": none of these lines was run by any test; Tests/policy_check.sh could only pin their text.)
//   python3 Tests/plugin_dlp_extract.py Sources/ctr/plugin.cpp "$OUT/plugin_dlp_slice.inc"
//   g++ -std=c++17 -O1 -g -Wall -Wextra -IIncludes -I"$OUT" -pthread -fsanitize=address,undefined
//       Tests/plugin_dlp_test.cpp Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp <CORE> <enet objects>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
    std::printf("plugin/the table of title ids: exactly these five, nothing else\n");
    struct Want {
        u64 tid;
        DlpGameMode mode;
    };
    // written out by hand (the mode of every title the plugin knows about; the comments of plugin.cpp say which are seen on a console)
    static const Want want[] = {
        {0x00040000001C2100ull, DlpGameMode::ProbeOnly},  // Kirby Battle Royale
        {0x00040000001C2000ull, DlpGameMode::ProbeOnly},  // Kirby Battle Royale, another region
        {0x000400000010C000ull, DlpGameMode::Lobby},      // Kirby Triple Deluxe
        {0x000400000010BF00ull, DlpGameMode::Lobby},      // Kirby Triple Deluxe, another region
        {0x0004000000183600ull, DlpGameMode::Lobby},      // Kirby Planet Robobot
    };
    CHECK_EQ(sizeof(kDlpGames) / sizeof(kDlpGames[0]), sizeof(want) / sizeof(want[0]));
    for (const Want& w : want) {
        CHECK(DlpModeOf(w.tid) == w.mode);
        CHECK_EQ(AsksDlpOnlyForTheWireless(w.tid), w.mode == DlpGameMode::ProbeOnly);
        // a title id that differs in ONE bit is another game: never in the table
        for (int bit = 0; bit < 64; ++bit) {
            const u64 other = w.tid ^ (1ull << bit);
            bool in_table = false;
            for (const Want& x : want) {
                in_table |= x.tid == other;
            }
            if (!in_table) {
                CHECK(DlpModeOf(other) == DlpGameMode::None);
                CHECK(!AsksDlpOnlyForTheWireless(other));
            }
        }
    }
    // games that must keep the real services (Tri Force Heroes, Mario Party Star Rush, Fire Emblem Fates, Pokemon, the empty id)
    static const u64 others[] = {0, 0x0004000000164800ull, 0x0004000000199300ull, 0x000400000017E600ull, 0x0004000000055D00ull, 0x00040000001B5100ull,
                                 0x000400000010C100ull, 0x0004000000183700ull, 0x00040000001C2200ull, 0xFFFFFFFFFFFFFFFFull};
    for (const u64 t : others) {
        CHECK(DlpModeOf(t) == DlpGameMode::None);
        CHECK(!AsksDlpOnlyForTheWireless(t));
    }
    // the hub exists only for Lobby: ProbeOnly and None have none (the plugin tests DlpModeOf(...) == Lobby)
    CHECK(DlpModeOf(0x00040000001C2100ull) != DlpGameMode::Lobby);
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
    std::printf("\n%d checks, %d failures\n", azl::ctr::g_checks, azl::ctr::g_failures);
    return azl::ctr::g_failures == 0 ? 0 : 1;
}
