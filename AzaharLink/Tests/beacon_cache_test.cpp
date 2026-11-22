// LAT-7 / UDS-BEACON: the host's beacon is not made again every 102 ms.
// (Written against 1.0.4 on 2026-10-26; carried over to 1.1.3 on 2026-11-10 with the networks that did not exist then: point 5.)
//
// The beacon of a host is a pure function of its NetworkInfo and its node table, and GenerateBeaconFrame costs a SHA-1, an MD5 and an
// AES-128-CTR in software (0.25-1.1 ms at 268 MHz, ten times a second, under the engine's lock). The engine keeps the last beacon together
// with the exact inputs it was made from (UdsEngine::BeaconFrameLocked). This test holds five things:
//   1. byte for byte: after every one of a few hundred random changes of the host's network (players that join and leave, application data,
//      the attribute word, a new network) the beacon that goes out equals GenerateBeaconFrame() of the engine's own inputs;
//   2. the work: SHA-1 runs once per distinct state of the network, not once per beacon (counted at the crypto function with the linker's
//      --wrap, so the same source runs against the engine before and after the change);
//   3. the pace is untouched (a beacon every 102.4 ms of the clock it is given, as TestBeaconTimer pins) and nothing is kept when no
//      network is hosted (the copy lives only while hosting).
//   4. the key is all the inputs: each byte of the NetworkInfo, each field of each node (the name, the seed, the id) and the number of the nodes,
//      changed ALONE in the engine's state, gives the beacon of the new inputs (the random changes of 1. alter several of them together).
//   5. the networks of 1.1.3: the real Download Play's (BeginHostingDlp: clients, spectators that are counted and never nodes, the ejects),
//      the fake lobby's (BeginHostingLobby), the host's odd DisconnectNetwork (it stays a host), one network after another on other
//      channels: the same two rules, and the frame around the body (broadcast, the network's channel) is made new every time.
//   6. no memory for the copy (2026-11-11, from the review of the change: the branch was run by no test): operator new(nothrow) fails
//      at the test's word, every beacon is still the right one and nothing is kept; when memory is back the copy is made and used.
//
// The test reads the engine's private state (white box, `#define private public`) because "the inputs of the beacon" are exactly
// network_info_ and node_info_; it needs no other change in the engine.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

#define private public
#include "support.hpp"
#undef private

static std::atomic<unsigned> g_sha1_calls{0};

// Linked with -Wl,--wrap=_ZN3azl4Sha1EPKhmPh (azl::Sha1): every call of the beacon's SHA-1 comes through here.
extern "C" void __real__ZN3azl4Sha1EPKhmPh(const unsigned char*, unsigned long, unsigned char*);
extern "C" void __wrap__ZN3azl4Sha1EPKhmPh(const unsigned char* data, unsigned long len, unsigned char* out) {
    ++g_sha1_calls;
    __real__ZN3azl4Sha1EPKhmPh(data, len, out);
}

// Point 6: the engine asks for the copy with new (std::nothrow), and this is that operator - it fails while the test says so.
static bool g_fail_nothrow = false;
static unsigned g_nothrow_failed = 0;
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (g_fail_nothrow) {
        ++g_nothrow_failed;
        return nullptr;
    }
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

namespace {

constexpr u32 kShared = 0x14000;

struct Host {
    Host() {
        mac = {0x00, 0x1F, 0x32, 0x00, 0x00, 0x01};
        UdsEngine::Transport tr;
        tr.send = [this](WifiPacket& p) {
            p.transmitter_address = mac;
            sent.push_back(p);
        };
        tr.get_mac = [this] { return mac; };
        engine.reset(new UdsEngine(&kernel, tr));
    }
    /// Makes the host's beacon timer fire once more (its schedule follows the clock it is given).
    void Beacon() {
        later_ms += 100000;
        engine->Tick(platform::NowMs() + later_ms);
    }
    /// The last beacon that went out.
    bool LastBeacon(std::vector<u8>* data) const {
        for (auto it = sent.rbegin(); it != sent.rend(); ++it) {
            if (it->type == WifiPacket::Type::Beacon) {
                *data = it->data;
                return true;
            }
        }
        return false;
    }
    std::size_t Beacons() const {
        std::size_t n = 0;
        for (const WifiPacket& p : sent) {
            n += p.type == WifiPacket::Type::Beacon;
        }
        return n;
    }
    /// A console that joins: its Authentication and its EAPoL Start, as the room hands them to the host.
    void Join(u8 last, u64 seed, const char* name) {
        const Mac who = {0x00, 0x1F, 0x32, 0x01, 0x00, last};
        WifiPacket auth;
        auth.type = WifiPacket::Type::Authentication;
        auth.transmitter_address = who;
        auth.destination_address = mac;
        auth.channel = 11;
        auth.data = GenerateAuthenticationFrame(AuthSeq::Seq1);
        engine->OnWifiPacket(auth);
        WifiPacket start;
        start.type = WifiPacket::Type::Data;
        start.transmitter_address = who;
        start.destination_address = mac;
        start.channel = 11;
        start.data = GenerateEapolStartFrame(1, ConnectionType::Client, MakeNode(seed, name, 0));
        engine->OnWifiPacket(start);
    }
    void Leave(u8 last) {
        WifiPacket bye;
        bye.type = WifiPacket::Type::Deauthentication;
        bye.transmitter_address = {0x00, 0x1F, 0x32, 0x01, 0x00, last};
        bye.destination_address = mac;
        bye.channel = 11;
        engine->OnWifiPacket(bye);
    }
    FakeKernel kernel;
    Mac mac;
    std::unique_ptr<UdsEngine> engine;
    std::vector<WifiPacket> sent;
    u64 later_ms = 0;
    u32 status_event = 0;
};

/// The inputs of the beacon, as one byte string (to count how many different states the network went through).
std::string InputsOf(const UdsEngine& e) {
    std::string s(reinterpret_cast<const char*>(&e.network_info_), sizeof(NetworkInfo));
    for (const NodeInfo& n : e.node_info_) {
        u8 raw[kNodeInfoSize];
        n.Store(raw);
        s.append(reinterpret_cast<const char*>(raw), sizeof(raw));
    }
    return s;
}

#ifdef AZL_HAS_BEACON_CACHE
// 6. No memory for the copy: the beacon is made every time, as before the change, and is the right one; nothing is kept and nothing is
// written through a null pointer (the `return` after the fallback, the copy of the key after the check); the copy comes when memory does.
void TestNoMemoryForTheCopy() {
    std::printf("beacon cache: no memory for the copy - every beacon made anew and right, the copy made when memory is back\n");
    Host h;
    CHECK_EQ(h.engine->Initialize(MakeNode(0x1001, "Host", 0), kShared, 0x400, &h.status_event), 0u);
    NetworkInfo ni = MakeNetworkInfo(4, 6);
    CHECK_EQ(h.engine->BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni)), 0u);
    const auto check = [&h](const char* what) {
        const std::size_t before = h.Beacons();
        h.Beacon();
        CHECK_EQ(h.Beacons(), before + 1);
        std::vector<u8> got;
        CHECK(h.LastBeacon(&got));
        if (!(got == GenerateBeaconFrame(h.engine->network_info_, h.engine->node_info_))) {
            std::printf("  a beacon that is not the one of its inputs: %s\n", what);
            CHECK(false);
        }
    };
    const unsigned failed_before = g_nothrow_failed;
    g_fail_nothrow = true;
    const u32 built = g_stats.beacon_built, reused = g_stats.beacon_reused;
    for (int i = 0; i < 5; ++i) {
        check("no memory");
        CHECK(h.engine->beacon_cache_ == nullptr);
    }
    static const u8 kData[3] = {1, 2, 3};
    CHECK_EQ(h.engine->SetApplicationData(kData, 3), 0u);
    check("no memory, after a change of the network");
    CHECK(h.engine->beacon_cache_ == nullptr);
    CHECK(g_nothrow_failed - failed_before >= 6); // (asked for again at every beacon)
    CHECK_EQ(g_stats.beacon_built - built, 6u);
    CHECK_EQ(g_stats.beacon_reused - reused, 0u);
    g_fail_nothrow = false;
    check("memory is back");
    CHECK(h.engine->beacon_cache_ != nullptr);
    CHECK_EQ(g_stats.beacon_built - built, 7u);
    check("memory is back, the second beacon");
    CHECK_EQ(g_stats.beacon_reused - reused, 1u);
    CHECK_EQ(h.engine->Shutdown(), 0u);
    CHECK(h.engine->beacon_cache_ == nullptr);
}
#endif

void TestByteForByte() {
    std::printf("beacon cache: random changes of the host's network, every beacon equals GenerateBeaconFrame of its inputs\n");
    std::mt19937 rng(0xBEAC0);
    Host h;
    CHECK_EQ(h.engine->Initialize(MakeNode(0x1001, "Host", 0), kShared, 0x400, &h.status_event), 0u);

    unsigned long beacons = 0;
    unsigned distinct = 0;
    std::string last_inputs;
    const unsigned sha_before = g_sha1_calls.load();
    unsigned networks = 0;
    for (unsigned step = 0; step < 400; ++step) {
        const unsigned op = h.engine->status() == NetworkStatus::ConnectedAsHost ? rng() % 8 : 0;
        switch (op) {
        case 0: { // a new network (the host stops hosting first if it is)
            if (h.engine->status() == NetworkStatus::ConnectedAsHost) {
                CHECK_EQ(h.engine->DestroyNetwork(), 0u);
            }
            NetworkInfo ni = MakeNetworkInfo(static_cast<u8>(2 + rng() % 15), static_cast<u8>(rng() % 0xC9));
            StoreBE32(ni.wlan_comm_id_be, rng());
            StoreBE32(ni.network_id_be, rng());
            ni.id = static_cast<u8>(rng());
            for (u32 i = 0; i < ni.application_data_size; ++i) {
                ni.application_data[i] = static_cast<u8>(rng());
            }
            CHECK_EQ(h.engine->BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni)), 0u);
            ++networks;
            break;
        }
        case 1:
        case 2:
        case 3: { // somebody joins
            h.Join(static_cast<u8>(2 + rng() % 12), rng() | (static_cast<u64>(rng()) << 32), step % 2 ? "Player" : "Caro");
            break;
        }
        case 4: { // somebody leaves
            h.Leave(static_cast<u8>(2 + rng() % 12));
            break;
        }
        case 5: { // the game changes what the lobby advertises
            std::vector<u8> data(rng() % 0xC9);
            for (u8& b : data) {
                b = static_cast<u8>(rng());
            }
            static const u8 kNone = 0;
            CHECK_EQ(h.engine->SetApplicationData(data.empty() ? &kNone : data.data(), static_cast<u32>(data.size())), 0u);
            break;
        }
        case 6: { // the attribute word
            CHECK_EQ(h.engine->UpdateNetworkAttribute(static_cast<u16>(1u << (rng() % 3)), static_cast<u8>(rng() % 2)), 0u);
            break;
        }
        default: // nothing changes: the beacons of a quiet lobby
            break;
        }
        // Between two changes the beacon goes out a few times
        const unsigned repeats = 1 + rng() % 4;
        for (unsigned r = 0; r < repeats; ++r) {
            const std::size_t before = h.Beacons();
            h.Beacon();
            CHECK_EQ(h.Beacons(), before + 1);
            std::vector<u8> got;
            CHECK(h.LastBeacon(&got));
            const std::vector<u8> expected = GenerateBeaconFrame(h.engine->network_info_, h.engine->node_info_);
            CHECK(got == expected);
            ++beacons;
            const std::string inputs = InputsOf(*h.engine);
            if (inputs != last_inputs) {
                ++distinct;
                last_inputs = inputs;
            }
        }
    }
    // SHA-1 ran for the beacons the engine made AND for the reference above (one per check): take the reference out
    const unsigned total = g_sha1_calls.load() - sha_before;
    const unsigned made_by_the_engine = total - static_cast<unsigned>(beacons);
    std::printf("  %lu beacons, %u different states of the network (%u networks hosted): SHA-1 ran %u times for the engine's beacons\n",
                beacons, distinct, networks, made_by_the_engine);
    // One generation per different state (a state is generated at its first beacon): the engine must not do more than that
    CHECK_EQ(made_by_the_engine, distinct);
}

void TestPaceAndMemory() {
    std::printf("beacon cache: the pace of the beacons is the same, and nothing is kept when no network is hosted\n");
    Host h;
    CHECK_EQ(h.engine->Initialize(MakeNode(0x1001, "Host", 0), kShared, 0x400, &h.status_event), 0u);
    NetworkInfo ni = MakeNetworkInfo(4, 6);
    CHECK_EQ(h.engine->BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni)), 0u);
    // 3 s of a loop that comes round every 3 ms, from the moment the network began
    const u64 t0 = platform::NowMs();
    std::vector<u64> at;
    for (u64 t = t0; t < t0 + 3000; t += 3) {
        const std::size_t before = h.Beacons();
        h.engine->Tick(t);
        if (h.Beacons() != before) {
            at.push_back(t);
        }
    }
    CHECK(at.size() >= 28 && at.size() <= 30); // 3000 / 102.4 = 29.3
    for (std::size_t i = 1; i < at.size(); ++i) {
        const u64 gap = at[i] - at[i - 1];
        CHECK(gap >= 100 && gap <= 106);
    }
    const unsigned sha = g_sha1_calls.load();
    h.Beacon();
    h.Beacon();
    h.Beacon();
    // the beacons of a lobby that does not change cost no SHA-1 at all
    CHECK_EQ(g_sha1_calls.load(), sha);
    // when the network ends nothing is kept (RAM: the copy is ~1 KB), and a client has none
#ifdef AZL_HAS_BEACON_CACHE
    CHECK(h.engine->beacon_cache_ != nullptr);
#endif
    CHECK_EQ(h.engine->DestroyNetwork(), 0u);
#ifdef AZL_HAS_BEACON_CACHE
    CHECK(h.engine->beacon_cache_ == nullptr);
#endif
    CHECK_EQ(h.engine->Shutdown(), 0u);
#ifdef AZL_HAS_BEACON_CACHE
    CHECK(h.engine->beacon_cache_ == nullptr);
    CHECK(g_stats.beacon_built >= 1 && g_stats.beacon_reused >= 3); // the counters of the log line
#endif
}

/// The key of the cache is ALL the inputs of the beacon. The random test above changes the network the way a game does (a join changes the seed, the id
/// and the name together), so a key that forgot the name, or the count of the nodes, would still pass it. Here each input changes ALONE, straight in
/// the engine's state, and the beacon that goes out next must be the one made from the new inputs.
void TestEveryInputIsInTheKey() {
    std::printf("beacon cache: each input of the beacon changed alone gives the beacon of the new inputs\n");
    Host h;
    CHECK_EQ(h.engine->Initialize(MakeNode(0x1001, "Host", 0), kShared, 0x400, &h.status_event), 0u);
    NetworkInfo ni = MakeNetworkInfo(6, 6);
    CHECK_EQ(h.engine->BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni)), 0u);
    h.Join(2, 0x2002, "Caro");
    h.Join(3, 0x2003, "Pablo");
    CHECK(h.engine->node_info_.size() >= 3); // the host and the two who joined
    unsigned stale = 0;
    auto current = [&](const char* what, unsigned long arg) {
        const std::size_t before = h.Beacons();
        h.Beacon();
        CHECK_EQ(h.Beacons(), before + 1);
        std::vector<u8> got;
        CHECK(h.LastBeacon(&got));
        if (!(got == GenerateBeaconFrame(h.engine->network_info_, h.engine->node_info_))) {
            ++stale;
            std::printf("  STALE beacon after: %s %lu\n", what, arg);
            CHECK(false);
        }
    };
    h.Beacon(); // the beacon of the quiet lobby: the cache is filled
    current("nothing changed", 0);

    // every byte of the NetworkInfo, alone (the bit that does not make the structure lie about its own size), and back
    u8* raw = reinterpret_cast<u8*>(&h.engine->network_info_);
    for (std::size_t i = 0; i < sizeof(NetworkInfo); ++i) {
        raw[i] ^= 1;
        current("network_info_ byte flipped:", static_cast<unsigned long>(i));
        raw[i] ^= 1;
        current("network_info_ byte restored:", static_cast<unsigned long>(i));
    }
    // 2026-11-10: the same with the application data full (200 bytes: every one of them is in the beacon, the last too; with the 6
    // bytes above a key that stopped one byte short of the NetworkInfo's end gave the same beacons and passed)
    h.engine->network_info_.application_data_size = kApplicationDataSize;
    for (std::size_t i = 0; i < kApplicationDataSize; ++i) {
        h.engine->network_info_.application_data[i] = static_cast<u8>(0x40 + i);
    }
    current("the application data filled", 0);
    for (std::size_t i = offsetof(NetworkInfo, application_data); i < sizeof(NetworkInfo); ++i) {
        raw[i] ^= 1;
        current("application data byte flipped (of 200):", static_cast<unsigned long>(i));
        raw[i] ^= 1;
        current("application data byte restored (of 200):", static_cast<unsigned long>(i));
    }
    // every node, field by field: the name (each of its ten characters), the seed, the id; and back each time (2026-11-10: without
    // the beacon of the way back, the restore of one field hid the change of the next: a key of nine characters of the name passed)
    for (std::size_t n = 0; n < h.engine->node_info_.size(); ++n) {
        for (int c = 0; c < 10; ++c) {
            h.engine->node_info_[n].username[c] ^= 1;
            current("a character of a name:", n * 10 + c);
            h.engine->node_info_[n].username[c] ^= 1;
            current("a character of a name restored:", n * 10 + c);
        }
        h.engine->node_info_[n].friend_code_seed ^= 1;
        current("a friend code seed:", n);
        h.engine->node_info_[n].friend_code_seed ^= 1;
        current("a friend code seed restored:", n);
        h.engine->node_info_[n].friend_code_seed ^= 1ull << 63; // (the other end of the seed)
        current("a friend code seed, top bit:", n);
        h.engine->node_info_[n].friend_code_seed ^= 1ull << 63;
        current("a friend code seed restored, top bit:", n);
        h.engine->node_info_[n].network_node_id ^= 1;
        current("a node id:", n);
        h.engine->node_info_[n].network_node_id ^= 1;
        current("a node id restored:", n);
        h.engine->node_info_[n].network_node_id ^= 0x8000;
        current("a node id, top bit:", n);
        h.engine->node_info_[n].network_node_id ^= 0x8000;
        current("a node id restored, top bit:", n);
    }
    current("everything back as it was", 0);
    // the number of the nodes, alone: one more (the table grows, the NetworkInfo says what it said), then one less, then two less
    const std::vector<NodeInfo> table = h.engine->node_info_;
    h.engine->node_info_.push_back(table.back());
    current("one more node:", h.engine->node_info_.size());
    h.engine->node_info_ = table;
    h.engine->node_info_.pop_back();
    current("one node less:", h.engine->node_info_.size());
    h.engine->node_info_.pop_back();
    current("two nodes less:", h.engine->node_info_.size());
    h.engine->node_info_ = table;
    current("the table as it was:", table.size());
    CHECK_EQ(stale, 0u);
}

/// 1.1.3: the networks the engine composes by itself and the calls that change a host's network without a join or a leave.
/// After every step a few beacons go out: each equals GenerateBeaconFrame of the engine's inputs at that moment, the frame around
/// it is the network's (broadcast, its channel), and SHA-1 ran once per state the inputs went through.
void TestNetworksOf113() {
    std::printf("beacon cache: the real Download Play's network, the fake lobby's, the host's DisconnectNetwork\n");
    Host h;
    static const u16 kName[10] = {'H', 'o', 's', 't'};
    unsigned long beacons = 0;
    unsigned distinct = 0;
    std::string last_inputs;
    const unsigned sha_before = g_sha1_calls.load();
    [[maybe_unused]] const u32 built_before = g_stats.beacon_built;
    [[maybe_unused]] const u32 reused_before = g_stats.beacon_reused;
    auto step = [&](const char* what, unsigned repeats) {
        for (unsigned r = 0; r < repeats; ++r) {
            const std::size_t before = h.Beacons();
            h.Beacon();
            CHECK_EQ(h.Beacons(), before + 1);
            const WifiPacket& last = h.sent.back();
            // (the last frame of the round is the beacon: nothing else is due)
            CHECK(last.type == WifiPacket::Type::Beacon);
            CHECK(last.destination_address == kBroadcastMac);
            CHECK_EQ(last.channel, h.engine->network_channel_);
            const std::vector<u8> expected = GenerateBeaconFrame(h.engine->network_info_, h.engine->node_info_);
            if (!(last.data == expected)) {
                std::printf("  STALE beacon after: %s\n", what);
                CHECK(false);
            }
            ++beacons;
            const std::string inputs = InputsOf(*h.engine);
            if (inputs != last_inputs) {
                ++distinct;
                last_inputs = inputs;
            }
        }
    };
    auto spectator = [&](u8 last) {
        const Mac who = {0x00, 0x1F, 0x32, 0x01, 0x00, last};
        WifiPacket auth;
        auth.type = WifiPacket::Type::Authentication;
        auth.transmitter_address = who;
        auth.destination_address = h.mac;
        auth.channel = 11;
        auth.data = GenerateAuthenticationFrame(AuthSeq::Seq1);
        h.engine->OnWifiPacket(auth);
        WifiPacket start;
        start.type = WifiPacket::Type::Data;
        start.transmitter_address = who;
        start.destination_address = h.mac;
        start.channel = 11;
        start.data = GenerateEapolStartFrame(1, ConnectionType::Spectator, MakeNode(0x5000 + last, "Spec", 0));
        h.engine->OnWifiPacket(start);
    };

    // the real Download Play's network on channel 6: a client, two spectators, the ejects, the goodbyes
    CHECK_EQ(h.engine->InitializeForLobby(0x1001, kName), 0u);
    CHECK_EQ(h.engine->BeginHostingDlp(4, 6), 0u);
    step("BeginHostingDlp", 3);
    h.Join(2, 0x2002, "Caro");
    step("a client of the Download Play", 2);
    spectator(7);
    step("a spectator", 2);
    spectator(8);
    step("a second spectator", 3);
    h.Leave(7);
    step("a spectator's goodbye", 2);
    CHECK_EQ(h.engine->EjectSpectators(), 0u);
    step("EjectSpectators", 2);
    h.Join(3, 0x2003, "Pablo");
    step("a second client", 2);
    CHECK_EQ(h.engine->EjectClient(2), 0u);
    step("EjectClient", 2);
    h.Leave(3);
    step("the last client's goodbye", 2);
#ifdef AZL_HAS_BEACON_CACHE
    CHECK(h.engine->beacon_cache_ != nullptr);
#endif
    CHECK_EQ(h.engine->Shutdown(), 0u); // (the Download Play ends with a Shutdown, not a DestroyNetwork)
#ifdef AZL_HAS_BEACON_CACHE
    CHECK(h.engine->beacon_cache_ == nullptr);
#endif
    {
        const std::size_t before = h.Beacons();
        h.Beacon();
        CHECK_EQ(h.Beacons(), before); // nobody hosts: no beacon, and nothing of the old network comes back
    }

    // the fake lobby's network on channel 1, with its application data; then the same console hosts another on channel 11
    CHECK_EQ(h.engine->InitializeForLobby(0x1001, kName), 0u);
    const u8 app[5] = {1, 2, 3, 4, 5};
    CHECK_EQ(h.engine->BeginHostingLobby(0x00040001, 0x55, 1, 4, app, sizeof(app)), 0u);
    step("BeginHostingLobby", 2);
    h.Join(2, 0x2002, "Caro");
    step("a client of the lobby", 2);
    CHECK_EQ(h.engine->DestroyNetwork(), 0u);
    CHECK_EQ(h.engine->BeginHostingLobby(0x00040001, 0x55, 11, 4, app, sizeof(app)), 0u);
    step("the lobby again on channel 11", 2);
    CHECK_EQ(h.engine->Shutdown(), 0u);

    // a game's network: DisconnectNetwork called by the HOST answers "wrong state", wipes the status block and leaves it a host
    CHECK_EQ(h.engine->Initialize(MakeNode(0x1001, "Host", 0), kShared, 0x400, &h.status_event), 0u);
    NetworkInfo ni = MakeNetworkInfo(4, 9);
    CHECK_EQ(h.engine->BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni)), 0u);
    step("BeginHostingNetwork", 2);
    h.Join(2, 0x2002, "Caro");
    step("a client of the game", 2);
    CHECK_EQ(h.engine->DisconnectNetwork(), res::kWrongState);
    CHECK(h.engine->status() == NetworkStatus::ConnectedAsHost);
    step("DisconnectNetwork as host", 3);
    h.Join(4, 0x2004, "Friend-A");
    step("a client after it", 2);
    CHECK_EQ(h.engine->Shutdown(), 0u);

    const unsigned made_by_the_engine = g_sha1_calls.load() - sha_before - static_cast<unsigned>(beacons);
    std::printf("  %lu beacons, %u different states of the network: SHA-1 ran %u times for the engine's beacons\n", beacons, distinct,
                made_by_the_engine);
    CHECK(distinct >= 10); // (a step that leaves the beacon's inputs alone - a spectator, the host's Disconnect - is no new state)
    CHECK_EQ(made_by_the_engine, distinct);
#ifdef AZL_HAS_BEACON_CACHE
    // the two counters of the log line say the same: one built per state, the rest from the copy
    CHECK_EQ(g_stats.beacon_built - built_before, distinct);
    CHECK_EQ(g_stats.beacon_reused - reused_before, static_cast<u32>(beacons) - distinct);
#endif
}

} // namespace

int main() {
    TestByteForByte();
    TestPaceAndMemory();
    TestEveryInputIsInTheKey();
    TestNetworksOf113();
#ifdef AZL_HAS_BEACON_CACHE
    TestNoMemoryForTheCopy();
#endif
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
