// The rig of the tests of the fake Download Play lobby over the REAL UDS engine (Tests/dlp_net_test.cpp, Tests/dlp_net_adv_test.cpp): engines joined by
// the in-memory room of Tests/mock_room.hpp, each with an EngineLobbyNet and a DlpLobby on top ("a Player"), a thread that ticks the engines (the
// beacons), and the helpers the scenarios use to drive a lobby the way the games do. Include it ONCE per test binary: it replaces the global
// allocation functions, to count the allocations of the calling thread (CountNews).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "azl/dlp_lobby_net.hpp"
#include "mock_room.hpp"

using namespace azl;


// ---------------------------------------------------------------------------------------------------------------------
// Allocations of the calling thread (the other threads of the rig - the room, the ticker - allocate as they like).

#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
static thread_local bool t_counting = false;
static thread_local unsigned t_news = 0;

void* operator new(std::size_t n) {
    if (t_counting) {
        ++t_news;
    }
    void* p = std::malloc(n ? n : 1);
    if (!p) {
        throw std::bad_alloc();
    }
    return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    if (t_counting) {
        ++t_news;
    }
    return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n) {
    return operator new(n);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
    return operator new(n, t);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

struct CountNews {
    CountNews() {
        t_news = 0;
        t_counting = true;
    }
    ~CountNews() {
        t_counting = false;
    }
    unsigned count() const {
        return t_news;
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Results and states written out (not the constants of the headers).

static constexpr u32 R_INVALID_STATE = 0xE0A0AC01;
static constexpr u32 R_BUSY = 0xE0A0AC03;
static constexpr u32 R_NO_DATA = 0xC880AFEF;
static constexpr u32 R_TIMEOUT = 0xC920AFFE;

static constexpr u32 kTidLow = 0x0010C000;  // Triple Deluxe's unique id 0x10C0
static const u8 kNine[9] = {'1', 'a', '2', 'b', '3', 'c', '4', 'd', 0};

static void Put(u16* dst, const char* s) {
    for (u32 i = 0; i < kNameUnits; ++i) {
        dst[i] = static_cast<u16>(s[i] ? static_cast<u8>(s[i]) : 0);
        if (!s[i]) {
            for (u32 j = i + 1; j < kNameUnits; ++j) {
                dst[j] = 0;
            }
            return;
        }
    }
}
static std::string Str(const u16* n) {
    std::string s;
    for (u32 i = 0; i < kNameUnits && n[i]; ++i) {
        s += static_cast<char>(n[i]);
    }
    return s;
}

class Ticker {
public:
    explicit Ticker(std::vector<UdsEngine*> engines) : engines_(std::move(engines)), thread_([this] { Run(); }) {}
    ~Ticker() {
        stop_ = true;
        thread_.join();
    }

private:
    void Run() {
        while (!stop_) {
            const u64 now = platform::NowMs();
            for (UdsEngine* e : engines_) {
                e->Tick(now);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    std::vector<UdsEngine*> engines_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

struct Player {
    Player(MockRoom& room, u8 last, const char* name, u32 tid_low = kTidLow)
        : con(room, last), net(con.engine.get()), lobby(&net, Cfg(tid_low, last, name)) {}
    static LobbyConfig Cfg(u32 tid_low, u8 last, const char* name) {
        LobbyConfig c;
        std::memset(&c, 0, sizeof(c));
        c.game_tid = tid_low;
        c.friend_seed = 0xF000u + last;
        Put(c.name, name);
        return c;
    }
    UdsEngine& e() {
        return *con.engine;
    }
    const Mac& mac() const {
        return con.mac;
    }
    Console con;
    EngineLobbyNet net;
    DlpLobby lobby;
};

using Players = std::vector<Player*>;

static std::vector<UdsEngine*> Engines(const Players& ps) {
    std::vector<UdsEngine*> v;
    for (Player* p : ps) {
        v.push_back(&p->e());
    }
    return v;
}

/// Pumps every lobby until `done()` or `ms` of real time have passed. True if done.
template <typename F>
static bool Until(const Players& ps, u32 ms, F done) {
    const u64 end = platform::NowMs() + ms;
    for (;;) {
        for (Player* p : ps) {
            p->lobby.Pump();
        }
        if (done()) {
            return true;
        }
        if (platform::NowMs() >= end) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
static void Settle(const Players& ps, u32 ms) {
    Until(ps, ms, [] { return false; });
}

static u32 CliState(Player& p) {
    return p.lobby.ClientStatusWord() >> 24;
}
static u32 CliNode(Player& p) {
    return p.lobby.ClientStatusWord() & 0xFF;
}

/// The creator's side as Triple Deluxe starts it.
static void OpenLobby(Player& host, u32 max_clients = 3, u32 channel = 6) {
    CHECK_EQ(host.lobby.ServerInitialize(max_clients, 0, nullptr), 0u);
    CHECK_EQ(host.lobby.ServerStartAccepting(false, channel), 0u);
}

/// The searcher listens until the creator's group is in its table: its Mac as the table has it.
static bool FindGroup(const Players& ps, Player& guest, Player& host, u32 ms = 4000) {
    CHECK_EQ(guest.lobby.ClientInitialize(nullptr, 4), 0u);
    CHECK_EQ(guest.lobby.ClientStartScan(0), 0u);
    return Until(ps, ms, [&] {
        GroupView v;
        return guest.lobby.ClientTitleInfo(host.mac(), &v) == 0;
    });
}

/// Searcher -> session (state 5): StartFakeSession as the IPC front-end does it.
static bool Join(const Players& ps, Player& guest, Player& host, u32* result, u32 ms = 4500) {
    guest.lobby.ClientStopScan();
    *result = 0xFFFFFFFF;
    if (guest.lobby.ClientJoinBegin(host.mac(), result) == JoinStep::Failed) {
        return true;  // *result written
    }
    return Until(ps, ms, [&] { return guest.lobby.ClientJoinPoll(result); });
}

static bool EngineClean(Player& p) {
    return !p.e().initialized() && !p.net.engine_held();
}

// ---------------------------------------------------------------------------------------------------------------------

/// A beacon as a host's engine would send it, composed by hand: `ids` are its network's, `app` its application data.
static WifiPacket MakeBeacon(const Mac& from, u32 wlan_comm_id, u8 id8, u8 total, u8 max, const u8* app, u32 n, u8 channel = 6) {
    NetworkInfo ni;
    std::memset(&ni, 0, sizeof(ni));
    std::memcpy(ni.oui_value, kNintendoOui, 3);
    ni.oui_type = 21;
    StoreBE32(ni.wlan_comm_id_be, wlan_comm_id);
    ni.id = id8;
    ni.total_nodes = total;
    ni.max_nodes = max;
    ni.application_data_size = static_cast<u8>(n);
    std::memcpy(ni.application_data, app, n);
    std::vector<NodeInfo> nodes(max);
    nodes[0] = MakeNode(0x77, "Host", 1);
    WifiPacket p;
    p.type = WifiPacket::Type::Beacon;
    p.data = GenerateBeaconFrame(ni, nodes);
    p.transmitter_address = from;
    p.destination_address = kBroadcastMac;
    p.channel = channel;
    return p;
}

/// A valid lobby frame as the room would hand it to `to`: channel 0xD1, node `from_node` -> `to_node`, sequence number `seq`.
static WifiPacket LobbyFrame(const Mac& transmitter, const Mac& to, u16 to_node, u16 from_node, u16 seq, const u8* body, u32 n) {
    WifiPacket p;
    p.type = WifiPacket::Type::Data;
    p.transmitter_address = transmitter;
    p.destination_address = to;
    p.channel = 11;
    // (the numbers a real engine gives its frames start from its clock and count up from there: these are far from them)
    p.data = GenerateDataPayload(body, n, UdsEngine::kLobbyChannel, to_node, from_node, static_cast<u16>(0xC000 | seq));
    return p;
}

// ---------------------------------------------------------------------------------------------------------------------
// More helpers (the adversarial scenarios of Tests/dlp_net_adv_test.cpp use them too)

[[maybe_unused]] static HostAnnounce Ann(u32 max_nodes = 4, u32 accepting = 1, const char* name = "Host") {
    HostAnnounce a;
    std::memset(&a, 0, sizeof(a));
    a.child_tid_low = kTidLow;
    a.max_nodes = static_cast<u8>(max_nodes);
    a.accepting = static_cast<u8>(accepting);
    Put(a.name, name);
    return a;
}
[[maybe_unused]] static Identity Me(u64 seed, const char* name) {
    Identity i;
    std::memset(&i, 0, sizeof(i));
    i.seed = seed;
    Put(i.name, name);
    return i;
}
template <typename F>
static bool WaitFor(u32 ms, F f) {
    const u64 end = platform::NowMs() + ms;
    for (;;) {
        if (f()) {
            return true;
        }
        if (platform::NowMs() >= end) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
static const Mac kFake1 = Mac{0x00, 0x1F, 0x32, 0x00, 0x77, 0x31};
static const Mac kFake2 = Mac{0x00, 0x1F, 0x32, 0x00, 0x77, 0x32};
static const u8 kMsg[2] = {1, 0};
