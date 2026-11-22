// The game's OWN nwm::UDS network with EIGHT nodes (2026-11-13): a host and seven clients, the network of Tetris ("nodes 1/8" in the
// console's log) and the one Mario Kart 7 opens after its Download Play lobby. No lobby here: only UdsEngine, as a game drives it
// (Initialize, BeginHostingNetwork with max_nodes 8, the scan, ConnectToNetwork, Bind / SendTo / PullPacket, GetConnectionStatus,
// GetNodeInformation, Disconnect, Destroy), over the in-memory room of the engine's tests (Tests/mock_room.hpp). Eight real nodes were
// never seen on consoles (five or six at the most), and the other engine tests stop at four.
//
//   1. the seven join one at a time: after EVERY join the tables of EVERY node are held (status, own id, total / max, the node mask,
//      nodes[], changed_nodes, GetNodeInformation of the 16 ids, NodeSnapshot, the signal table, the status event), and so is the
//      host's beacon as a console that only scans reads it (two encrypted tags: the eighth node's entry lies across both);
//   2. a ninth console is not let in, and nobody's table moves;
//   3. every node sends a broadcast and a frame to every other node: each one arrives once, where it must and nowhere else, by every
//      way a frame has (the node's MAC, the broadcast flag with and without the room's member list, the host's relay);
//   4. a client leaves, comes back, two leave and a new console takes a free id, a client leaves the room, one is ejected;
//   5. the host ends the network and everybody is told;
//   6. the seven join in the same round trip (threads); 6b. seven AZAHAR clients ask in the same round trip (they take the first
//      Logoff they hear): with the switch of the title (Mario Kart 7) each has its own id, without it the frames of before;
//   7. six guests that say goodbye twice (Azahar's) leave and come back together;
//   8. the cost: eight nodes that each broadcast a 300-byte frame 60 times a second - frames and bytes through the room, and what the
//      receive ring of a bind does with Tetris's size when the game stops reading for a while.
//
// KNOWN_GAP_EQ marks the places where the engine of TODAY does not do what a console does (open points, not corrected by this test:
// analysis/lobby-otto-2026-11-13/revisione-uds-normale-a-otto/RAPPORTO.md). It holds what the engine does today, so the test is
// green; the day a point is closed its line fails and says so, and is to be turned into a CHECK_EQ of the console's value.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "azl/log.hpp"
#include "azl/room_proto.hpp"
#include "azl/stats.hpp"
#include "azl/uds_engine.hpp"
#include "azl/uds_frames.hpp"

using namespace azl;

#include "support.hpp"
#include "mock_room.hpp"
#include "azahar_peer.hpp"

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

namespace {

constexpr u32 kHostStatus = 6;   // NetworkStatus::ConnectedAsHost
constexpr u32 kClientStatus = 9; // NetworkStatus::ConnectedAsClient
constexpr u32 kIdleStatus = 3;   // NetworkStatus::NotConnected
constexpr u8 kMaxNodes = 8;
constexpr u8 kDataChannel = 1;
constexpr u32 kBindId = 2;
constexpr u32 kTetrisRecvBuffer = 11824; // what Tetris gives Bind (console log of 2026-11-13: "recv buffer 11824, ring 16384 bytes")
constexpr u32 kFrameBytes = 300;
constexpr u32 kWireHead = 19; // room::EncodeWifiPacket: id, type, channel, two MACs, a 32-bit length

int g_known_gaps = 0;

#define KNOWN_GAP_EQ(id, got, console, today)                                                                             \
    do {                                                                                                                 \
        ++g_checks;                                                                                                      \
        const long long kg_got = static_cast<long long>(got);                                                            \
        const long long kg_console = static_cast<long long>(console);                                                    \
        const long long kg_today = static_cast<long long>(today);                                                        \
        if (kg_got == kg_today) {                                                                                        \
            ++g_known_gaps;                                                                                              \
        } else {                                                                                                         \
            ++g_failures;                                                                                                \
            std::fprintf(stderr, "  FAIL %s:%d: known gap %s: %s is %lld (today %lld, a console %lld)%s\n", __FILE__,     \
                         __LINE__, id, #got, kg_got, kg_today, kg_console,                                               \
                         kg_got == kg_console ? ": the point was CLOSED - turn this line into a CHECK_EQ" : "");         \
        }                                                                                                                \
    } while (0)

/// The 5 s of ConnectToNetwork would make the refused ninth a long test (the engine waits in slices: the cap is the whole wait's).
struct CappedKernel : FakeKernel {
    bool WaitEvent(u32 h, s64 ns) override {
        return FakeKernel::WaitEvent(h, std::min<s64>(ns, cap_ns));
    }
    bool WaitEventGuarded(u32 h, s64 ns) override {
        return FakeKernel::WaitEventGuarded(h, std::min<s64>(ns, cap_ns));
    }
    s64 cap_ns = 5000000000LL;
};

/// One console in the room: an engine, its kernel, what its game holds (the status event, a bind) and what the room carried for it.
struct Station {
    Station(MockRoom& room, u8 mac_last, const char* name_) : name(name_), seed(0x8000u + mac_last), console(room, mac_last, &kernel) {
        console.drop = [this](const WifiPacket& p) { return lose_node_maps.load() && p.type == WifiPacket::Type::NodeMap; };
        console.tap = [this](const WifiPacket& p) {
            ++rx_frames;
            rx_bytes += kWireHead + p.data.size();
        };
        console.sniff = [this](const WifiPacket& p) {
            ++tx_frames;
            tx_bytes += kWireHead + p.data.size();
        };
    }
    UdsEngine& e() {
        return *console.engine;
    }
    const Mac& mac() const {
        return console.mac;
    }
    u32 Init() {
        return e().Initialize(MakeNode(seed, name, 0), kTestSharedMemory, kTestVersion, &status_event);
    }
    u32 Host(u8 max_nodes) {
        NetworkInfo ni = MakeNetworkInfo(max_nodes, 6);
        return e().BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni));
    }
    u32 BindAll() {
        return e().Bind(kBindId, kTetrisRecvBuffer, kDataChannel, kBroadcastNetworkNodeId, &bind_event);
    }
    /// The status event's signals since the last call.
    u32 NewEvents() {
        const u32 n = kernel.Signals(status_event);
        const u32 d = n - events_seen;
        events_seen = n;
        return d;
    }
    u16 NodeId() {
        u16 id = 0, mask = 0;
        e().NodeSnapshot(&id, &mask);
        return id;
    }

    const char* name;
    u64 seed;
    CappedKernel kernel;
    Console console;
    u32 status_event = 0;
    u32 bind_event = 0;
    u32 events_seen = 0;
    std::atomic<bool> lose_node_maps{false};
    std::atomic<u64> rx_frames{0}, rx_bytes{0}, tx_frames{0}, tx_bytes{0};
};

/// Who is in the network: slot n-1 holds the console that is node n.
struct Roster {
    Station* slot[kUdsMaxNodes] = {};
    u16 Mask() const {
        u16 m = 0;
        for (u32 i = 0; i < kUdsMaxNodes; ++i) {
            m |= slot[i] ? static_cast<u16>(1u << i) : 0;
        }
        return m;
    }
    int Count() const {
        return __builtin_popcount(Mask());
    }
    int IdOf(const Station* s) const {
        for (u32 i = 0; i < kUdsMaxNodes; ++i) {
            if (slot[i] == s) {
                return static_cast<int>(i + 1);
            }
        }
        return 0;
    }
};

void Hold(const char* when, const char* who, const char* what, long long got, long long want) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL [%s] %s: %s is %lld, not %lld\n", when, who, what, got, want);
    }
}

constexpr int kAnyChange = -1;

/// The tables of every node of the roster against the roster. `changed` says the changed_nodes word each one must read (kAnyChange =
/// not looked at); reading the status clears it, as it does for a game.
void CheckTables(const char* when, const Roster& roster, const std::function<int(Station&)>& changed) {
    const u16 mask = roster.Mask();
    const int count = roster.Count();
    for (u32 i = 0; i < kUdsMaxNodes; ++i) {
        Station* s = roster.slot[i];
        if (!s) {
            continue;
        }
        const ConnectionStatus st = s->e().GetConnectionStatus();
        Hold(when, s->name, "status", st.status, i == 0 ? kHostStatus : kClientStatus);
        Hold(when, s->name, "network_node_id", st.network_node_id, i + 1);
        Hold(when, s->name, "total_nodes", st.total_nodes, count);
        Hold(when, s->name, "max_nodes", st.max_nodes, kMaxNodes);
        Hold(when, s->name, "node_bitmask", st.node_bitmask, mask);
        for (u32 n = 0; n < kUdsMaxNodes; ++n) {
            Hold(when, s->name, "nodes[]", st.nodes[n], roster.slot[n] ? n + 1 : 0);
        }
        const int want_changed = changed(*s);
        if (want_changed != kAnyChange) {
            Hold(when, s->name, "changed_nodes", st.changed_nodes, want_changed);
        }
        Hold(when, s->name, "changed_nodes read again", s->e().GetConnectionStatus().changed_nodes, 0);
        for (u16 id = 1; id <= kUdsMaxNodes; ++id) {
            NodeInfo info;
            const u32 r = s->e().GetNodeInformation(id, &info);
            const Station* at = roster.slot[id - 1];
            Hold(when, s->name, "GetNodeInformation result", r, at ? 0u : res::kNodeNotFound);
            if (at && r == 0) {
                Hold(when, s->name, "node info: seed", static_cast<long long>(info.friend_code_seed), static_cast<long long>(at->seed));
                Hold(when, s->name, "node info: id", info.network_node_id, id);
                bool name_ok = true;
                for (int c = 0; c < 10; ++c) {
                    const u16 want = c < static_cast<int>(std::strlen(at->name)) ? static_cast<u16>(at->name[c]) : 0;
                    name_ok = name_ok && info.username[c] == want;
                }
                Hold(when, s->name, "node info: name", name_ok, 1);
            }
        }
        u16 my = 0, snap = 0;
        Hold(when, s->name, "NodeSnapshot", s->e().NodeSnapshot(&my, &snap), 1);
        Hold(when, s->name, "NodeSnapshot id", my, i + 1);
        Hold(when, s->name, "NodeSnapshot mask", snap, mask);
        u16 signal[UdsEngine::kPeerSignalEntries];
        Hold(when, s->name, "GetPeerSignal", s->e().GetPeerSignal(signal), 0);
        for (u32 n = 0; n < kUdsMaxNodes; ++n) {
            Hold(when, s->name, "signal table", signal[1 + n] != 0, roster.slot[n] && n != i);
        }
    }
}

/// Makes the host's engine send a beacon now (its timer runs on the time it is given).
void Beacon(Station& host) {
    static u64 later_ms = 0;
    later_ms += 100000;
    host.e().Tick(platform::NowMs() + later_ms);
}

/// What a console that scans reads of the first network: the NetworkInfo a game builds of it, the node list its DecryptBeaconData
/// gives (through the two 0xFE-byte tag buffers the SDK hands over) and how many encrypted tags the beacon has.
bool ScanNetwork(Station& s, NetworkInfo* net, std::vector<NodeInfo>* nodes, int* encrypted_tags) {
    std::vector<u8> scan(0x4000);
    if (s.e().RecvBeaconBroadcastData(scan.data(), static_cast<u32>(scan.size()), kBroadcastMac) != 0 ||
        !FirstScannedNetwork(scan, net)) {
        return false;
    }
    if (!nodes) {
        return true;
    }
    const u8* entry = scan.data() + kBeaconDataReplyHeaderSize;
    const u32 entry_size = LoadLE32(entry);
    const u8* beacon = entry + kBeaconEntryHeaderSize;
    const std::size_t beacon_size = entry_size - kBeaconEntryHeaderSize;
    u8 tag[2][0xFE] = {};
    *encrypted_tags = 0;
    for (std::size_t pos = 12; pos + 2 <= beacon_size;) {
        const u8 id = beacon[pos], len = beacon[pos + 1];
        pos += 2;
        if (id == 0xdd && len >= 4 && pos + len <= beacon_size && std::memcmp(beacon + pos, kNintendoOui, 3) == 0 &&
            (beacon[pos + 3] == 24 || beacon[pos + 3] == 25)) {
            std::memcpy(tag[beacon[pos + 3] - 24], beacon + pos, std::min<std::size_t>(len, 0xFE));
            ++*encrypted_tags;
        }
        pos += len;
    }
    std::vector<u8> raw;
    if (s.e().DecryptBeaconData(*net, tag[0], sizeof(tag[0]), tag[1], sizeof(tag[1]), &raw) != 0 ||
        raw.size() != kNodeInfoSize * kUdsMaxNodes) {
        return false;
    }
    nodes->clear();
    for (u32 i = 0; i < kUdsMaxNodes; ++i) {
        NodeInfo n;
        n.Load(raw.data() + i * kNodeInfoSize);
        nodes->push_back(n);
    }
    return true;
}

/// The host sends a beacon and `s` scans (a scan lists a host whose beacon came in the 102 ms before it began: on a machine that
/// stalled in between, the beacon is sent again, a few times).
bool ScanFresh(MockRoom& room, Station& host, Station& s, NetworkInfo* net, std::vector<NodeInfo>* nodes, int* encrypted_tags) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        Beacon(host);
        room.WaitIdle();
        if (ScanNetwork(s, net, nodes, encrypted_tags)) {
            return true;
        }
    }
    return false;
}

/// `client` scans for the host's beacon and joins, as a game does.
u32 Join(MockRoom& room, Station& client, Station& host) {
    NetworkInfo net;
    if (!ScanFresh(room, host, client, &net, nullptr, nullptr)) {
        return 0xFFFFFFFF;
    }
    const u32 r = client.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1);
    room.WaitIdle();
    return r;
}

struct Got {
    u16 src;
    std::vector<u8> bytes;
};

/// Everything queued in the console's bind.
std::vector<Got> Drain(Station& s) {
    std::vector<Got> out;
    for (int i = 0; i < 4096; ++i) {
        std::vector<u8> buffer;
        u32 size = 0;
        u16 src = 0;
        if (PullInto(s.e(), kBindId, &buffer, &size, &src) != 0 || size == 0) {
            break;
        }
        buffer.resize(size);
        out.push_back({src, std::move(buffer)});
    }
    return out;
}

std::vector<u8> Payload(u8 kind, u8 src, u8 dest, u8 round) {
    std::vector<u8> p(kFrameBytes);
    p[0] = kind;
    p[1] = src;
    p[2] = dest;
    p[3] = round;
    for (u32 i = 4; i < kFrameBytes; ++i) {
        p[i] = static_cast<u8>(i * 7 + src * 31 + round);
    }
    return p;
}

/// Every node of the roster sends one frame to everybody (node id 0xFFFF): each of the others must find exactly one frame of each of
/// the others, whole, and the sender none of its own.
void CheckBroadcastOnce(const char* when, MockRoom& room, const Roster& roster, u8 round) {
    for (u32 i = 0; i < kUdsMaxNodes; ++i) {
        if (Station* s = roster.slot[i]) {
            const std::vector<u8> p = Payload('B', static_cast<u8>(i + 1), 0xFF, round);
            Hold(when, s->name, "SendTo(0xFFFF)", s->e().SendTo(kBroadcastNetworkNodeId, kDataChannel, kFrameBytes, 0, p.data(), p.size()), 0);
        }
    }
    room.WaitIdle();
    for (u32 i = 0; i < kUdsMaxNodes; ++i) {
        Station* s = roster.slot[i];
        if (!s) {
            continue;
        }
        const std::vector<Got> got = Drain(*s);
        Hold(when, s->name, "broadcasts received", static_cast<long long>(got.size()), roster.Count() - 1);
        int seen[kUdsMaxNodes + 1] = {};
        for (const Got& g : got) {
            const bool known = g.src >= 1 && g.src <= kUdsMaxNodes && roster.slot[g.src - 1] && g.src != i + 1;
            Hold(when, s->name, "broadcast: a node of the network", known, 1);
            if (known) {
                ++seen[g.src];
                Hold(when, s->name, "broadcast: bytes", g.bytes == Payload('B', static_cast<u8>(g.src), 0xFF, round), 1);
            }
        }
        for (u32 n = 1; n <= kUdsMaxNodes; ++n) {
            Hold(when, s->name, "broadcast: copies of one node", seen[n], roster.slot[n - 1] && n != i + 1 ? 1 : 0);
        }
    }
}

/// Every node sends one frame to every other node (`flags` 0, or 2 = the broadcast flag with a node in mind): the addressee must find
/// exactly one frame of each of the others, and nobody a frame that was not for it.
void CheckUnicastOnce(const char* when, MockRoom& room, const Roster& roster, u8 flags, u8 round) {
    for (u32 i = 0; i < kUdsMaxNodes; ++i) {
        for (u32 j = 0; j < kUdsMaxNodes; ++j) {
            Station* s = roster.slot[i];
            if (!s || !roster.slot[j] || i == j) {
                continue;
            }
            const std::vector<u8> p = Payload('U', static_cast<u8>(i + 1), static_cast<u8>(j + 1), round);
            Hold(when, s->name, "SendTo(node)", s->e().SendTo(static_cast<u16>(j + 1), kDataChannel, kFrameBytes, flags, p.data(), p.size()), 0);
        }
    }
    room.WaitIdle();
    for (u32 i = 0; i < kUdsMaxNodes; ++i) {
        Station* s = roster.slot[i];
        if (!s) {
            continue;
        }
        const std::vector<Got> got = Drain(*s);
        Hold(when, s->name, "unicasts received", static_cast<long long>(got.size()), roster.Count() - 1);
        int seen[kUdsMaxNodes + 1] = {};
        for (const Got& g : got) {
            const bool known = g.src >= 1 && g.src <= kUdsMaxNodes && roster.slot[g.src - 1] && g.src != i + 1;
            Hold(when, s->name, "unicast: a node of the network", known, 1);
            if (known) {
                ++seen[g.src];
                Hold(when, s->name, "unicast: bytes, and for this node",
                     g.bytes == Payload('U', static_cast<u8>(g.src), static_cast<u8>(i + 1), round), 1);
            }
        }
        for (u32 n = 1; n <= kUdsMaxNodes; ++n) {
            Hold(when, s->name, "unicast: copies of one node", seen[n], roster.slot[n - 1] && n != i + 1 ? 1 : 0);
        }
    }
}

std::vector<Mac> MacsOf(const std::vector<Station*>& stations) {
    std::vector<Mac> macs;
    for (const Station* s : stations) {
        macs.push_back(s->mac());
    }
    return macs;
}

/// A guest driven by hand, for the frames an engine never sends (Azahar's second goodbye).
class Guest {
public:
    Guest(MockRoom& room, u8 mac_last) : mac{0x00, 0x1F, 0x32, 0x00, 0x00, mac_last}, room_(room) {
        index_ = room.AddMember(mac, [](const WifiPacket&) {});
    }
    ~Guest() {
        room_.RemoveMember(index_);
    }
    void Send(WifiPacket::Type type, const Mac& to, std::vector<u8> data) {
        WifiPacket p;
        p.type = type;
        p.channel = 11;
        p.transmitter_address = mac;
        p.destination_address = to;
        p.data = std::move(data);
        g_rules.Inspect(p);
        room_.Send(index_, p);
    }
    void Join(const Mac& host) {
        Send(WifiPacket::Type::Authentication, host, GenerateAuthenticationFrame(AuthSeq::Seq1));
        room_.WaitIdle();
        Send(WifiPacket::Type::Data, host, GenerateEapolStartFrame(1, ConnectionType::Client, MakeNode(0x7000 + mac[5], "Guest", 0)));
        room_.WaitIdle();
    }
    void Goodbye(const Mac& host) {
        Send(WifiPacket::Type::Deauthentication, host, {});
    }
    Mac mac;

private:
    MockRoom& room_;
    int index_ = -1;
};

} // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// 1 - 5: one evening of eight
// ---------------------------------------------------------------------------------------------------------------------------------
static void TestEightNodes() {
    std::printf("eight nodes: the joins one at a time, the ninth, the frames, who leaves and who comes back, the host's end\n");
    g_rules.Reset();
    std::memset(&g_stats, 0, sizeof(g_stats));
    MockRoom room;
    static const char* const kNames[] = {"Host", "C2", "C3", "C4", "C5", "C6", "C7", "C8"};
    std::vector<std::unique_ptr<Station>> node;
    for (u8 i = 0; i < 8; ++i) {
        node.emplace_back(new Station(room, static_cast<u8>(1 + i), kNames[i]));
    }
    Station ninth(room, 9, "Ninth"), watcher(room, 10, "Watcher");
    ninth.kernel.cap_ns = 300000000; // (its join is refused by silence: 0.3 s instead of 5)
    Station& host = *node[0];
    std::vector<Station*> everyone;
    for (auto& s : node) {
        everyone.push_back(s.get());
        CHECK_EQ(s->Init(), 0u);
    }
    everyone.push_back(&ninth);
    everyone.push_back(&watcher);
    CHECK_EQ(ninth.Init(), 0u);
    CHECK_EQ(watcher.Init(), 0u);
    // The room tells every console who is in it, as the plugin's room client does (the first list concludes nothing).
    for (Station* s : everyone) {
        s->e().OnRoomMembers(MacsOf(everyone));
    }

    // ---- 1. the host, then one join at a time -----------------------------------------------------------------------------------
    Roster roster;
    CHECK_EQ(host.Host(kMaxNodes), 0u);
    roster.slot[0] = &host;
    CheckTables("the host alone", roster, [](Station&) { return 0x0001; });
    CHECK_EQ(host.NewEvents(), 1u);
    for (u32 k = 2; k <= 8; ++k) {
        Station& joiner = *node[k - 1];
        char when[48];
        std::snprintf(when, sizeof(when), "node %u joined", static_cast<unsigned>(k));
        CHECK_EQ(Join(room, joiner, host), 0u);
        roster.slot[k - 1] = &joiner;
        // the newcomer reads every node as changed, the others the newcomer alone
        CheckTables(when, roster, [&](Station& s) { return &s == &joiner ? roster.Mask() : 1 << (k - 1); });
        for (u32 n = 1; n <= k; ++n) {
            Hold(when, node[n - 1]->name, "status events", node[n - 1]->NewEvents(), 1); // one-shot: once per join, for everybody
        }
        // the beacon, as a console outside the network reads it
        NetworkInfo net;
        std::vector<NodeInfo> listed;
        int encrypted_tags = 0;
        CHECK(ScanFresh(room, host, watcher, &net, &listed, &encrypted_tags));
        Hold(when, "beacon", "total_nodes", net.total_nodes, k);
        Hold(when, "beacon", "max_nodes", net.max_nodes, kMaxNodes);
        // 8 node entries of 30 bytes and 18 of header are 258: more than the 250 of one tag. The eighth entry lies across the two.
        Hold(when, "beacon", "encrypted tags", encrypted_tags, 2);
        for (u32 n = 1; n <= kUdsMaxNodes && listed.size() == kUdsMaxNodes; ++n) {
            Hold(when, "beacon", "node list: id", listed[n - 1].network_node_id, n <= k ? n : 0);
            Hold(when, "beacon", "node list: seed", static_cast<long long>(listed[n - 1].friend_code_seed),
                 n <= k ? static_cast<long long>(node[n - 1]->seed) : 0);
            if (n <= k) {
                bool name_ok = true;
                for (int c = 0; c < 10; ++c) {
                    const u16 want = c < static_cast<int>(std::strlen(node[n - 1]->name)) ? static_cast<u16>(node[n - 1]->name[c]) : 0;
                    name_ok = name_ok && listed[n - 1].username[c] == want;
                }
                Hold(when, "beacon", "node list: name", name_ok, 1);
            }
        }
    }
    CHECK_EQ(roster.Count(), 8);
    CHECK_EQ(roster.Mask(), 0x00FF);

    // ---- 2. the ninth -----------------------------------------------------------------------------------------------------------
    // A full host answers nothing (Azahar's sends no reject frame and asserts on a Start it cannot take), so the join ends as the
    // one to a host that is not there: 0xC92113FE after the wait. A console's nwm answers 0xC8611001 at once (audit of 2026-10-18,
    // A2+GB-07, left as it is on purpose: the right code cannot be made without a reject frame). A game that scans first sees 8/8.
    for (Station* s : everyone) {
        s->NewEvents();
    }
    const u32 ninth_frames = static_cast<u32>(ninth.tx_frames.load());
    KNOWN_GAP_EQ("A2+GB-07 (a full host)", Join(room, ninth, host), 0xC8611001u, res::kConnectTimeout);
    CHECK(ninth.e().status() == NetworkStatus::NotConnected);
    // (the pre-Deauthentication of a start over and the Authentication: no EAPoL-Start goes to a full host - the rule checker too)
    CHECK_EQ(static_cast<u32>(ninth.tx_frames.load()) - ninth_frames, 2u);
    CheckTables("the ninth was refused", roster, [](Station&) { return 0; });
    for (u32 n = 1; n <= 8; ++n) {
        Hold("the ninth was refused", node[n - 1]->name, "status events", node[n - 1]->NewEvents(), 0);
    }

    // ---- 3. the frames ----------------------------------------------------------------------------------------------------------
    for (auto& s : node) {
        CHECK_EQ(s->BindAll(), 0u);
    }
    Stats before = g_stats;
    CheckBroadcastOnce("broadcast of eight", room, roster, 1);
    CHECK_EQ(g_stats.data_delivered - before.data_delivered, 56u); // 8 senders x 7 receivers
    CHECK_EQ(g_stats.forwarded - before.forwarded, 0u);            // (a frame to the broadcast address has reached everybody: no relay)
    before = g_stats;
    CheckUnicastOnce("unicast of eight", room, roster, 0, 2);
    CHECK_EQ(g_stats.data_delivered - before.data_delivered, 56u);
    CHECK_EQ(g_stats.forwarded - before.forwarded, 0u);            // every node knows the MAC of every other: straight to it
    CHECK_EQ(g_stats.drop_not_for_me - before.drop_not_for_me, 0u);
    // The broadcast flag with a node in mind (Heroes of Ruin, FFE): to that node's MAC while the room lists it ...
    before = g_stats;
    CheckUnicastOnce("broadcast flag, addressed", room, roster, 2, 3);
    CHECK_EQ(g_stats.sendto_addressed - before.sendto_addressed, 56u);
    CHECK_EQ(g_stats.data_delivered - before.data_delivered, 56u);
    CHECK_EQ(g_stats.drop_not_for_me - before.drop_not_for_me, 0u);
    // ... and to everybody when the room's list is not known (a join of the room just made): six of the eight drop each frame
    for (auto& s : node) {
        s->e().ResetRoomPresence();
    }
    before = g_stats;
    CheckUnicastOnce("broadcast flag, to the room", room, roster, 2, 4);
    CHECK_EQ(g_stats.sendto_addressed - before.sendto_addressed, 0u);
    CHECK_EQ(g_stats.data_delivered - before.data_delivered, 56u);
    CHECK_EQ(g_stats.drop_not_for_me - before.drop_not_for_me, 56u * 6u);
    CHECK_EQ(g_stats.forwarded - before.forwarded, 0u); // (the host does not relay what was sent to the broadcast address)
    CHECK_EQ(g_stats.drop_fwd_dup - before.drop_fwd_dup, 0u);
    CHECK_EQ(g_stats.ring_overwritten, 0u);
    CHECK_EQ(g_stats.drop_dup_copy, 0u);
    CHECK_EQ(g_stats.drop_no_bind, 0u);
    for (Station* s : everyone) {
        s->e().OnRoomMembers(MacsOf(everyone));
    }

    // ---- 4a. a client leaves the network and stays in the room (it went back to the game's menu) -------------------------------
    for (auto& s : node) {
        s->e().GetConnectionStatus();
        s->NewEvents();
    }
    Station& c4 = *node[3];
    const u32 c4_bind_signals = c4.kernel.Signals(c4.bind_event);
    CHECK_EQ(c4.e().DisconnectNetwork(), 0u);
    room.WaitIdle();
    {
        const ConnectionStatus st = c4.e().GetConnectionStatus();
        CHECK_EQ(st.status, kIdleStatus);
        CHECK_EQ(st.status_change_reason, 2u); // RequestFromMyself
        CHECK_EQ(st.network_node_id, 4);       // (kept: a game still reads it after it left)
        CHECK_EQ(st.total_nodes, 0);
        CHECK_EQ(st.node_bitmask, 0);
        CHECK_EQ(c4.NewEvents(), 1u);
        CHECK_EQ(c4.kernel.Signals(c4.bind_event) - c4_bind_signals, 1u);
    }
    roster.slot[3] = nullptr;
    {
        // the host knows at once
        const ConnectionStatus st = host.e().GetConnectionStatus();
        CHECK_EQ(st.total_nodes, 7);
        CHECK_EQ(st.node_bitmask, 0x00F7);
        CHECK_EQ(st.changed_nodes, 0x0008);
        CHECK_EQ(st.nodes[3], 0);
        CHECK_EQ(host.NewEvents(), 1u);
        NodeInfo info;
        CHECK_EQ(host.e().GetNodeInformation(4, &info), res::kNodeNotFound);
    }
    // GB-01 (audit of 2026-10-18, still open): the OTHER clients are told nothing. A console compares every beacon of its host with
    // its node table (the beacon lists 7 nodes from now on) and drops the node; here the engine's host sends no frame about a
    // departure in a game's network, and a client's beacon comparison only counts ("beacon sync (shadow)").
    for (u32 n : {2u, 3u, 5u, 6u, 7u, 8u}) {
        Station& s = *node[n - 1];
        const ConnectionStatus st = s.e().GetConnectionStatus();
        CHECK_EQ(st.total_nodes, 7); // (closed 2026-11-15, decision D5: GB-01)
        CHECK_EQ(st.node_bitmask, 0x00F7); // (closed 2026-11-15, decision D5: GB-01)
        CHECK_EQ(st.changed_nodes, 0x0008); // (closed 2026-11-15, decision D5: GB-01)
        CHECK_EQ(st.nodes[3], 0); // (closed 2026-11-15, decision D5: GB-01)
        CHECK_EQ(s.NewEvents(), 1); // (closed 2026-11-15, decision D5: GB-01)
        NodeInfo info;
        CHECK_EQ(s.e().GetNodeInformation(4, &info), res::kNodeNotFound); // (closed 2026-11-15, decision D5: GB-01)
    }
    // ... and the host's beacons do say it: ten a second, each one disagreeing with the table the clients keep
    {
        const u32 disagree = g_stats.sync_disagree;
        for (int i = 0; i < 3; ++i) {
            Beacon(host);
            room.WaitIdle();
        }
        CHECK_EQ(g_stats.sync_disagree - disagree, 0u); // (GB-01 closed, 2026-11-15: the host told the clients, their tables agree with its beacons)
        NetworkInfo net;
        std::vector<NodeInfo> listed;
        int tags = 0;
        CHECK(ScanFresh(room, host, watcher, &net, &listed, &tags));
        CHECK_EQ(net.total_nodes, 7);
        CHECK_EQ(listed.size() == kUdsMaxNodes ? listed[3].network_node_id : 99, 0);
    }
    // The seven that are left still reach each other once; a frame for the node that left goes nowhere.
    CheckBroadcastOnce("broadcast of seven", room, roster, 5);
    CheckUnicastOnce("unicast of seven", room, roster, 0, 6);
    for (u32 n : {1u, 2u, 8u}) {
        const std::vector<u8> p = Payload('U', static_cast<u8>(n), 4, 7);
        CHECK_EQ(node[n - 1]->e().SendTo(4, kDataChannel, kFrameBytes, 0, p.data(), p.size()), 0u);
    }
    room.WaitIdle();
    for (auto& s : node) {
        CHECK_EQ(Drain(*s).size(), std::size_t(0));
    }

    // ---- 4b. it comes back: the lowest free id is its old one, and the admission tells everybody -------------------------------
    for (auto& s : node) {
        s->e().GetConnectionStatus();
        s->NewEvents();
    }
    CHECK_EQ(Join(room, c4, host), 0u);
    roster.slot[3] = &c4;
    CHECK_EQ(c4.NodeId(), 4);
    for (u32 n : {2u, 3u, 5u, 6u, 7u, 8u}) {
        // (a console saw the node go and come: bit 3. Here the mask had the bit before and has it now: "nothing changed")
        CHECK_EQ(node[n - 1]->e().GetConnectionStatus().changed_nodes, 0x0008); // (closed 2026-11-15, decision D5: GB-01 (left and came back))
    }
    CheckTables("node 4 is back", roster, [&](Station& s) { return &s == &c4 ? 0x00FF : &s == &host ? 0x0008 : 0; });
    for (u32 n : {2u, 3u, 5u, 6u, 7u, 8u}) {
        CHECK_EQ(node[n - 1]->NewEvents(), 1u); // the event comes ...
    }
    CHECK_EQ(host.NewEvents(), 1u);
    CHECK_EQ(c4.NewEvents(), 1u);
    CheckBroadcastOnce("broadcast of eight again", room, roster, 8);
    CheckUnicastOnce("unicast of eight again", room, roster, 0, 9);

    // ---- 4c. two leave, a console that was never in takes the lowest free id -----------------------------------------------------
    for (auto& s : node) {
        s->e().GetConnectionStatus();
        s->NewEvents();
    }
    Station& c3 = *node[2];
    Station& c6 = *node[5];
    CHECK_EQ(c3.e().DisconnectNetwork(), 0u);
    CHECK_EQ(c6.e().DisconnectNetwork(), 0u);
    room.WaitIdle();
    roster.slot[2] = nullptr;
    roster.slot[5] = nullptr;
    CHECK_EQ(host.e().GetConnectionStatus().node_bitmask, 0x00DB);
    CHECK_EQ(host.NewEvents(), 2u);
    ninth.kernel.cap_ns = 5000000000LL;
    CHECK_EQ(Join(room, ninth, host), 0u); // (the console that was refused a moment ago)
    CHECK_EQ(ninth.NodeId(), 3);
    roster.slot[2] = &ninth;
    CHECK_EQ(ninth.BindAll(), 0u);
    // Everybody's table is right again: the admission's Logoff carries the whole list. What the clients that stayed read as changed:
    // node 6 went (they learn it now), and node 3 is ANOTHER console - a console saw it go and saw the new one come, bit 2 is set
    // twice over; here the mask had the bit before and has it now, so the word does not say that the node is somebody else.
    for (u32 n : {2u, 4u, 5u, 7u, 8u}) {
        CHECK_EQ(node[n - 1]->e().GetConnectionStatus().changed_nodes, 0x0024); // (closed 2026-11-15, decision D5: GB-01 (the node is another console))
    }
    CheckTables("a new console is node 3", roster, [&](Station& s) {
        return &s == &ninth ? roster.Mask() : &s == &host ? 0x0004 : 0;
    });
    for (u32 n : {2u, 4u, 5u, 7u, 8u}) {
        CHECK_EQ(node[n - 1]->NewEvents(), 3u); // (GB-01 closed: the two departures and the admission, each told, as a console tells them)
    }
    // the console that was node 3 comes back and is node 6
    CHECK_EQ(Join(room, c3, host), 0u);
    CHECK_EQ(c3.NodeId(), 6);
    roster.slot[5] = &c3;
    CheckTables("the old node 3 is node 6", roster, [&](Station& s) {
        return &s == &c3 ? roster.Mask() : 0x0020; // (this one every client reads: they had learnt that node 6 was gone)
    });
    CheckBroadcastOnce("broadcast after the changes", room, roster, 10);
    CheckUnicastOnce("unicast after the changes", room, roster, 0, 11);

    // The word itself, on a fresh pair of departures: node 5 leaves and the console that is out (C6) takes its place before any
    // client read its status.
    for (u32 i = 0; i < kUdsMaxNodes; ++i) {
        if (roster.slot[i]) {
            roster.slot[i]->e().GetConnectionStatus();
            roster.slot[i]->NewEvents();
        }
    }
    Station& c5 = *node[4];
    CHECK_EQ(c5.e().DisconnectNetwork(), 0u);
    room.WaitIdle();
    roster.slot[4] = nullptr;
    CHECK_EQ(Join(room, c6, host), 0u);
    CHECK_EQ(c6.NodeId(), 5);
    roster.slot[4] = &c6;
    for (u32 i = 1; i < kUdsMaxNodes; ++i) {
        Station* s = roster.slot[i];
        if (!s || s == &c6) {
            continue;
        }
        const ConnectionStatus st = s->e().GetConnectionStatus();
        CHECK_EQ(st.node_bitmask, 0x00FF);
        NodeInfo info;
        CHECK_EQ(s->e().GetNodeInformation(5, &info), 0u);
        CHECK_EQ(info.friend_code_seed, c6.seed); // the table is the new console's ...
        CHECK_EQ(st.changed_nodes, 0x0010); // (closed 2026-11-15, decision D5: GB-01 (the node is another console)) - and the word says so
        CHECK_EQ(s->NewEvents(), 2u); // (GB-01 closed: the departure and the admission, each told)
    }
    CHECK_EQ(host.e().GetConnectionStatus().changed_nodes, 0x0010);
    CheckTables("C6 is node 5", roster, [&](Station& s) { return &s == &c6 ? 0x00FF : 0; });
    c5.kernel.cap_ns = 300000000;
    KNOWN_GAP_EQ("A2+GB-07 (a full host)", Join(room, c5, host), 0xC8611001u, res::kConnectTimeout); // full again: C5 stays out
    CHECK(c5.e().status() == NetworkStatus::NotConnected);

    // ---- 4d. a client leaves the ROOM (console off, link lost): no goodbye; every node drops it by itself after 3 s -------------
    for (u32 i = 0; i < 8; ++i) {
        roster.slot[i]->e().GetConnectionStatus();
        roster.slot[i]->NewEvents();
    }
    Station* gone = roster.slot[7]; // node 8
    std::vector<Station*> still;
    for (Station* s : everyone) {
        if (s != gone) {
            still.push_back(s);
        }
    }
    roster.slot[7] = nullptr;
    CHECK(gone->e().OnTransportDown(NetworkStatusChangeReason::ConnectionLost)); // (its own side: the link is gone, no frame leaves)
    for (u32 i = 0; i < 7; ++i) {
        roster.slot[i]->e().OnRoomMembers(MacsOf(still));
        roster.slot[i]->e().Tick(platform::NowMs() + 1500);
    }
    Hold("gone from the room for 1.5 s", "Host", "total_nodes", host.e().GetConnectionStatus().total_nodes, 8);
    for (u32 i = 0; i < 7; ++i) {
        roster.slot[i]->e().Tick(platform::NowMs() + 4000);
    }
    CheckTables("node 8 left the room", roster, [](Station&) { return 0x0080; });
    for (u32 i = 0; i < 7; ++i) {
        Hold("node 8 left the room", roster.slot[i]->name, "status events", roster.slot[i]->NewEvents(), 1);
    }
    // by presence on the host; on each client either by presence too or by the host's refresh first (GB-01 closed, 2026-11-15): whichever comes first
    CHECK(g_stats.node_removed[3] >= 1u && g_stats.node_removed[3] <= 7u);
    room.WaitIdle();
    CheckBroadcastOnce("broadcast of seven, one out of the room", room, roster, 12);
    CheckUnicastOnce("unicast of seven, one out of the room", room, roster, 0, 13);

    // ---- 4e. the host ejects a client: it is told, the others are not (GB-01 again) ----------------------------------------------
    Station* ejected = roster.slot[6]; // node 7
    CHECK_EQ(host.e().EjectClient(7), 0u);
    room.WaitIdle();
    roster.slot[6] = nullptr;
    {
        const ConnectionStatus st = ejected->e().GetConnectionStatus();
        CHECK_EQ(st.status, kIdleStatus);
        CHECK_EQ(st.status_change_reason, 4u); // DiscardedFromNetwork
        CHECK_EQ(ejected->NewEvents(), 1u);
        const ConnectionStatus hs = host.e().GetConnectionStatus();
        CHECK_EQ(hs.total_nodes, 6);
        CHECK_EQ(hs.node_bitmask, 0x003F);
        CHECK_EQ(hs.changed_nodes, 0x0040);
        CHECK_EQ(host.NewEvents(), 1u);
    }
    for (u32 i = 1; i < 6; ++i) {
        const ConnectionStatus st = roster.slot[i]->e().GetConnectionStatus();
        CHECK_EQ(st.total_nodes, 6); // (closed 2026-11-15, decision D5: GB-01 (EjectClient))
        CHECK_EQ(st.node_bitmask, 0x003F); // (closed 2026-11-15, decision D5: GB-01 (EjectClient))
        CHECK_EQ(roster.slot[i]->NewEvents(), 1); // (closed 2026-11-15, decision D5: GB-01 (EjectClient))
    }
    CheckBroadcastOnce("broadcast of six", room, roster, 14);
    CheckUnicastOnce("unicast of six", room, roster, 0, 15);

    // ---- 5. the host ends the network: every client is out, told once, its bind woken once ---------------------------------------
    u32 bind_signals[kUdsMaxNodes] = {};
    for (u32 i = 1; i < 6; ++i) {
        bind_signals[i] = roster.slot[i]->kernel.Signals(roster.slot[i]->bind_event);
    }
    CHECK_EQ(host.e().DestroyNetwork(), 0u);
    room.WaitIdle();
    CHECK_EQ(host.e().GetConnectionStatus().status, kIdleStatus);
    CHECK_EQ(host.NewEvents(), 1u);
    for (u32 i = 1; i < 6; ++i) {
        Station& s = *roster.slot[i];
        const ConnectionStatus st = s.e().GetConnectionStatus();
        Hold("the host ended the network", s.name, "status", st.status, kIdleStatus);
        Hold("the host ended the network", s.name, "reason", st.status_change_reason, 4);
        Hold("the host ended the network", s.name, "total_nodes", st.total_nodes, 0);
        Hold("the host ended the network", s.name, "node_bitmask", st.node_bitmask, 0);
        Hold("the host ended the network", s.name, "own id kept", st.network_node_id, i + 1);
        Hold("the host ended the network", s.name, "status events", s.NewEvents(), 1); // (three Deauthentications, one end)
        Hold("the host ended the network", s.name, "bind woken", s.kernel.Signals(s.bind_event) - bind_signals[i], 1);
        u32 size = 1;
        u16 src = 0;
        std::vector<u8> buffer;
        Hold("the host ended the network", s.name, "PullPacket", PullInto(s.e(), kBindId, &buffer, &size, &src), res::kWrongState);
    }
    CHECK_EQ(g_stats.ring_overwritten, 0u);
    CHECK_EQ(g_stats.drop_dup_copy, 0u);
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// 3 (the host's relay): a client that never got the host's node map knows no MAC but the host's; its frames for another client go to
// the host, which sends them on to the room once
// ---------------------------------------------------------------------------------------------------------------------------------
static void TestEightThroughTheHost() {
    std::printf("eight nodes: a client without the node map sends through the host, once\n");
    g_rules.Reset();
    std::memset(&g_stats, 0, sizeof(g_stats));
    MockRoom room;
    static const char* const kNames[] = {"Host", "C2", "C3", "C4", "C5", "C6", "C7", "C8"};
    std::vector<std::unique_ptr<Station>> node;
    Roster roster;
    for (u8 i = 0; i < 8; ++i) {
        node.emplace_back(new Station(room, static_cast<u8>(1 + i), kNames[i]));
        CHECK_EQ(node[i]->Init(), 0u);
    }
    node[7]->lose_node_maps = true;
    CHECK_EQ(node[0]->Host(kMaxNodes), 0u);
    roster.slot[0] = node[0].get();
    for (u32 k = 2; k <= 8; ++k) {
        CHECK_EQ(Join(room, *node[k - 1], *node[0]), 0u);
        roster.slot[k - 1] = node[k - 1].get();
    }
    CheckTables("eight, one without the node map", roster, [](Station&) { return kAnyChange; });
    for (auto& s : node) {
        CHECK_EQ(s->BindAll(), 0u);
    }
    const Stats before = g_stats;
    CheckUnicastOnce("unicast of eight, one through the host", room, roster, 0, 1);
    CHECK_EQ(g_stats.data_delivered - before.data_delivered, 56u);
    CHECK_EQ(g_stats.forwarded - before.forwarded, 6u);         // node 8 to nodes 2..7
    CHECK_EQ(g_stats.fwd_unmatched - before.fwd_unmatched, 6u); // ... each the only copy there is
    CHECK_EQ(g_stats.drop_echo - before.drop_echo, 6u);         // ... and back at node 8, dropped
    CHECK_EQ(g_stats.drop_not_for_me - before.drop_not_for_me, 6u * 5u);
    CHECK_EQ(g_stats.drop_fwd_dup - before.drop_fwd_dup, 0u);
    CheckBroadcastOnce("broadcast of eight, one without the node map", room, roster, 2);
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// 6: the seven join in the same round trip (seven game threads in ConnectToNetwork at once), three times over
// ---------------------------------------------------------------------------------------------------------------------------------
static void TestEightAtOnce() {
    std::printf("eight nodes: the seven clients join at the same time\n");
    static const char* const kNames[] = {"Host", "C2", "C3", "C4", "C5", "C6", "C7", "C8"};
    for (int round = 0; round < 3; ++round) {
        g_rules.Reset();
        MockRoom room;
        std::vector<std::unique_ptr<Station>> node;
        for (u8 i = 0; i < 8; ++i) {
            node.emplace_back(new Station(room, static_cast<u8>(1 + i), kNames[i]));
            CHECK_EQ(node[i]->Init(), 0u);
        }
        Station& host = *node[0];
        CHECK_EQ(host.Host(kMaxNodes), 0u);
        NetworkInfo net[8];
        for (u32 i = 1; i < 8; ++i) {
            CHECK(ScanFresh(room, host, *node[i], &net[i], nullptr, nullptr));
        }
        std::atomic<bool> go{false};
        u32 result[8] = {};
        std::vector<std::thread> threads;
        for (u32 i = 1; i < 8; ++i) {
            threads.emplace_back([&, i] {
                while (!go.load()) {
                    std::this_thread::yield();
                }
                result[i] = node[i]->e().ConnectToNetwork(reinterpret_cast<u8*>(&net[i]), sizeof(net[i]), 1);
            });
        }
        go = true;
        for (std::thread& t : threads) {
            t.join();
        }
        room.WaitIdle();
        Roster roster;
        roster.slot[0] = &host;
        for (u32 i = 1; i < 8; ++i) {
            CHECK_EQ(result[i], 0u);
            const u16 id = node[i]->NodeId();
            CHECK(id >= 2 && id <= 8);
            if (id >= 2 && id <= 8) {
                CHECK(roster.slot[id - 1] == nullptr); // nobody shares an id
                roster.slot[id - 1] = node[i].get();
            }
        }
        CHECK_EQ(roster.Mask(), 0x00FF);
        if (roster.Mask() == 0x00FF) {
            CheckTables("seven joined at once", roster, [](Station&) { return kAnyChange; });
            for (auto& s : node) {
                CHECK_EQ(s->BindAll(), 0u);
            }
            CheckBroadcastOnce("broadcast after joining at once", room, roster, 1);
            CheckUnicastOnce("unicast after joining at once", room, roster, 0, 2);
        }
        CHECK_RULES();
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// 6b: seven AZAHAR players ask in the same round trip (Mario Kart 7 after its Download Play: the seven of the capture of 2026-11-13
// were back within a tenth of a second). Azahar's nwm takes the FIRST Logoff it hears while it is Connecting as its own, whoever it
// names (Tests/azahar_peer.hpp, AzaharClient; nwm_uds.cpp, HandleEAPoLPacket): a host that broadcasts the Logoff of every admission -
// Azahar's own, and ours for every game since 1.0 - gives them all the id of the first (in that capture three of seven had another's
// id, and the two whose id was taken fell out of the race). The engine's switch of the title (SetAdmissionToMembers: the plugin turns
// it on for Mario Kart 7 only) sends each admission to the members of the network one by one, as in the real Download Play's network:
// everybody gets its own id. With the switch off the frames are the ones of before, and the defect is there: both are held.
// ---------------------------------------------------------------------------------------------------------------------------------
static void TestAzaharAskTogether() {
    std::printf("eight nodes: seven Azahar clients ask at the same time, without and with the title's switch\n");
    for (const bool to_members : {false, true}) {
        g_rules.Reset();
        MockRoom room;
        Station host(room, 1, "Host");
        std::mutex m;
        std::map<Mac, u16> given; // the host's last node map
        std::map<Mac, u32> logoffs_to;
        u32 logoffs_of_node_0 = 0;
        host.console.sniff = [&](const WifiPacket& p) {
            std::lock_guard<std::mutex> l(m);
            if (p.type == WifiPacket::Type::NodeMap) {
                std::vector<NodeMapEntry> entries;
                if (ParseNodeMapPacket(p.data, &entries)) {
                    given.clear();
                    for (const NodeMapEntry& e : entries) {
                        given[e.mac] = e.node_id;
                    }
                }
                return;
            }
            EtherType type;
            EapolLogoff logoff;
            if (p.type == WifiPacket::Type::Data && GetFrameEtherType(p.data, &type) && type == EtherType::EAPoL &&
                ParseEapolLogoffFrame(p.data, &logoff)) {
                ++logoffs_to[p.destination_address];
                logoffs_of_node_0 += logoff.assigned_node_id == 0 ? 1u : 0u;
            }
        };
        // the frames to the host wait at its door until the test opens it: the seven requests are in the room's queue before the host
        // answers the first, whatever the threads do
        std::atomic<bool> door{true};
        host.console.drop = [&](const WifiPacket&) {
            while (!door.load()) {
                std::this_thread::yield();
            }
            return false;
        };
        CHECK(!host.e().admission_to_members()); // an engine nobody told: off
        host.e().SetAdmissionToMembers(to_members);
        // (on a console the Download Play's lobby has the engine first and gives it back with a Shutdown: the switch is the title's and stays)
        CHECK_EQ(host.Init(), 0u);
        CHECK_EQ(host.e().Shutdown(), 0u);
        CHECK_EQ(host.e().admission_to_members(), to_members);
        CHECK_EQ(host.Init(), 0u);
        CHECK_EQ(host.Host(kMaxNodes), 0u);
        CHECK_EQ(host.e().admission_to_members(), to_members);
        NetworkInfo net = MakeNetworkInfo(kMaxNodes, 6);
        net.host_mac_address = host.mac();

        // a spectator first: its Logoff names node 0 - broadcast, it is what a client that is Connecting must never hear
        AzaharClient watcher(room, 0x30, MakeNode(0x5030, "Watcher", 0));
        watcher.Connect(net, ConnectionType::Spectator);
        room.WaitIdle();
        CHECK(watcher.status() == NetworkStatus::ConnectedAsSpectator);
        {
            std::lock_guard<std::mutex> l(m);
            CHECK_EQ(logoffs_of_node_0, 1u);
            CHECK_EQ(logoffs_to[kBroadcastMac], to_members ? 0u : 1u);
            CHECK_EQ(logoffs_to[watcher.mac()], to_members ? 1u : 0u);
            logoffs_to.clear();
        }

        std::vector<std::unique_ptr<AzaharClient>> az;
        for (u8 i = 0; i < 7; ++i) {
            const std::string name = "Az" + std::to_string(i + 2);
            az.emplace_back(new AzaharClient(room, static_cast<u8>(0x12 + i), MakeNode(0x5000u + i, name.c_str(), 0)));
        }
        door = false;
        for (auto& a : az) {
            a->Connect(net);
        }
        door = true;
        room.WaitIdle();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        room.WaitIdle();

        u32 wrong = 0, in = 0, ids = 0;
        std::lock_guard<std::mutex> l(m);
        for (u8 i = 0; i < 7; ++i) {
            const u16 gave = given.count(az[i]->mac()) ? given[az[i]->mac()] : 0;
            CHECK_EQ(gave, i + 2u); // the host admits them in the order they asked: nodes 2..8
            in += az[i]->connected() ? 1u : 0u;
            wrong += az[i]->node_id() != gave ? 1u : 0u;
            ids |= 1u << az[i]->node_id();
        }
        CHECK_EQ(in, 7u);
        CHECK_EQ(host.e().GetConnectionStatus().total_nodes, 8u);
        CHECK_EQ(host.e().GetConnectionStatus().node_bitmask, 0x00FFu);
        CHECK_EQ(host.e().SpectatorCount(), 1u);
        u32 to_one = 0, to_strangers = 0;
        for (const auto& entry : logoffs_to) {
            if (entry.first == kBroadcastMac) {
                continue;
            }
            to_one += entry.second;
            to_strangers += entry.first != watcher.mac() && given.count(entry.first) == 0 ? entry.second : 0u;
        }
        std::printf("  switch %s: %u of 7 Azahar clients believe they are another's node; admission Logoffs: %u broadcast, %u to one member\n",
                    to_members ? "on" : "off", wrong, logoffs_to[kBroadcastMac], to_one);
        if (to_members) {
            // to the members one by one: nobody hears another's admission before its own - every Azahar has the id the host gave it
            CHECK_EQ(wrong, 0u);
            CHECK_EQ(ids, 0x01FCu); // 2..8, each once
            CHECK_EQ(logoffs_to[kBroadcastMac], 0u);
            CHECK_EQ(to_one, 35u); // the k-th admission to the spectator and the k clients that are in: 2 + 3 + ... + 8
            CHECK_EQ(to_strangers, 0u);
            CHECK_EQ(logoffs_to[watcher.mac()], 7u);
        } else {
            // as since 1.0 (and as Azahar's own host): ONE broadcast for each admission, nothing else - and the seven Azahar all take the first
            CHECK_EQ(logoffs_to[kBroadcastMac], 7u);
            CHECK_EQ(to_one, 0u);
            CHECK_EQ(wrong, 6u);
            CHECK_EQ(ids, 0x0004u); // every one of them "node 2"
        }
        CHECK_EQ(logoffs_of_node_0, 1u); // (the spectator's, above: no other admission names node 0)
        CHECK_RULES();
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// 7: Azahar's guests say goodbye twice, up to 2 s apart; a host remembers who left lately so that the second copy does not end the
// association of a guest that came back at once (Node::deauth_guard). The table of who left has FOUR places.
// ---------------------------------------------------------------------------------------------------------------------------------
static void TestLateGoodbyesOfMany() {
    std::printf("eight nodes: guests that leave and come back together, and the late second copy of their goodbye\n");
    for (const u32 guests : {4u, 6u}) {
        g_rules.Reset();
        std::memset(&g_stats, 0, sizeof(g_stats));
        MockRoom room;
        Station host(room, 1, "Host");
        CHECK_EQ(host.Init(), 0u);
        CHECK_EQ(host.Host(kMaxNodes), 0u);
        std::vector<std::unique_ptr<Guest>> guest;
        for (u32 i = 0; i < guests; ++i) {
            guest.emplace_back(new Guest(room, static_cast<u8>(0x20 + i)));
            guest[i]->Join(host.mac());
        }
        CHECK_EQ(host.e().GetConnectionStatus().total_nodes, 1 + guests);
        for (auto& g : guest) {
            g->Goodbye(host.mac()); // the first copy: they all leave (a game that takes everybody back to its lobby)
        }
        room.WaitIdle();
        CHECK_EQ(host.e().GetConnectionStatus().total_nodes, 1u);
        for (auto& g : guest) {
            g->Join(host.mac()); // ... and are back at once
        }
        CHECK_EQ(host.e().GetConnectionStatus().total_nodes, 1 + guests);
        for (auto& g : guest) {
            g->Goodbye(host.mac()); // the late second copy of each goodbye
        }
        room.WaitIdle();
        const ConnectionStatus st = host.e().GetConnectionStatus();
        if (guests <= 4) {
            CHECK_EQ(st.total_nodes, 1 + guests); // every copy swallowed
            CHECK_EQ(g_stats.deauth_late, guests);
        } else {
            // The fifth and the sixth departure took the places of the first and the second in the table of four: those two guests
            // came back unguarded and their late copies end their new associations. The guests believe they are in.
            CHECK_EQ(st.total_nodes, 1 + guests); // (closed 2026-11-15, decision D5: kLeftTable)
            CHECK_EQ(g_stats.deauth_late, guests); // (closed 2026-11-15, decision D5: kLeftTable)
            CHECK_EQ(st.node_bitmask, 0x007F); // (closed 2026-11-15, decision D5: kLeftTable)
        }
        CHECK_RULES();
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// 8: the cost. No threads and no real time here: the engines hand their frames to a queue, the queue is emptied to the members the
// way the room does it and the plugin takes them (the prefilter on the encoded frame first, the decoder for what it lets through).
// ---------------------------------------------------------------------------------------------------------------------------------
namespace {

struct LoadNet {
    struct Member {
        Mac mac;
        FakeKernel kernel;
        std::unique_ptr<UdsEngine> engine;
        u32 status_event = 0;
        u32 bind_event = 0;
        u64 rx_frames = 0, rx_bytes = 0, tx_frames = 0, tx_bytes = 0, tx_beacons = 0, tx_beacon_bytes = 0, prefiltered = 0;
        u64 pulled = 0;
    };
    explicit LoadNet(u32 nodes) {
        for (u32 i = 0; i < nodes; ++i) {
            m.emplace_back(new Member());
            Member* me = m.back().get();
            me->mac = {0x00, 0x1F, 0x32, 0x00, 0x01, static_cast<u8>(1 + i)};
            UdsEngine::Transport tr;
            tr.send = [this, me, i](WifiPacket& p) {
                p.transmitter_address = me->mac;
                std::vector<u8> bytes = room::EncodeWifiPacket(p);
                ++me->tx_frames;
                me->tx_bytes += bytes.size();
                if (p.type == WifiPacket::Type::Beacon) {
                    ++me->tx_beacons;
                    me->tx_beacon_bytes += bytes.size();
                }
                queue.push_back({i, std::move(bytes)});
            };
            tr.get_mac = [me] { return me->mac; };
            me->engine.reset(new UdsEngine(&me->kernel, tr));
        }
    }
    /// The room: a frame for the broadcast address goes to every member but its sender, any other to the member with that MAC.
    void Pump() {
        while (!queue.empty()) {
            std::pair<u32, std::vector<u8>> item = std::move(queue.front());
            queue.pop_front();
            const std::vector<u8>& b = item.second;
            Mac dest;
            std::memcpy(dest.data(), b.data() + 9, 6);
            for (u32 i = 0; i < m.size(); ++i) {
                if (dest == kBroadcastMac ? i == item.first : m[i]->mac != dest) {
                    continue;
                }
                ++m[i]->rx_frames;
                m[i]->rx_bytes += b.size();
                if (m[i]->engine->TryDiscardEncoded(b.data(), b.size())) {
                    ++m[i]->prefiltered;
                    continue;
                }
                WifiPacket p;
                if (room::DecodeWifiPacket(b.data(), b.size(), &p)) {
                    m[i]->engine->OnWifiPacket(p);
                }
            }
        }
    }
    /// A host and its clients, each with Tetris's bind.
    bool Form() {
        bool ok = true;
        for (u32 i = 0; i < m.size(); ++i) {
            ok = ok && m[i]->engine->Initialize(MakeNode(0x9000 + i, "Load", 0), kTestSharedMemory, kTestVersion, &m[i]->status_event) == 0;
        }
        NetworkInfo ni = MakeNetworkInfo(static_cast<u8>(m.size()), 6);
        ok = ok && m[0]->engine->BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni)) == 0;
        for (u32 i = 1; i < m.size(); ++i) {
            NetworkInfo net = MakeNetworkInfo(static_cast<u8>(m.size()), 6);
            net.host_mac_address = m[0]->mac;
            net.channel = 11;
            ok = ok && m[i]->engine->ConnectBegin(reinterpret_cast<u8*>(&net), sizeof(net), 1) == 0;
            Pump();
            ok = ok && m[i]->engine->ConnectPoll() == 1;
        }
        for (auto& me : m) {
            ok = ok && me->engine->Bind(kBindId, kTetrisRecvBuffer, kDataChannel, kBroadcastNetworkNodeId, &me->bind_event) == 0;
            me->rx_frames = me->rx_bytes = me->tx_frames = me->tx_bytes = me->tx_beacons = me->tx_beacon_bytes = me->prefiltered = 0;
        }
        return ok;
    }
    /// Takes up to `limit` frames out of the member's bind (the frames must be whole and in each sender's order).
    u32 Pull(u32 i, u32 limit, bool* in_order) {
        u32 n = 0;
        for (; n < limit; ++n) {
            std::vector<u8> buffer;
            u32 size = 0;
            u16 src = 0;
            if (PullInto(*m[i]->engine, kBindId, &buffer, &size, &src) != 0 || size == 0) {
                break;
            }
            u32 tick = 0;
            std::memcpy(&tick, buffer.data() + 4, 4);
            *in_order = *in_order && size == kFrameBytes && src >= 1 && src <= m.size() && buffer[1] == src &&
                        (last_tick[i][src] == 0 || tick > last_tick[i][src]);
            last_tick[i][src] = tick;
        }
        m[i]->pulled += n;
        return n;
    }
    std::vector<std::unique_ptr<Member>> m;
    std::deque<std::pair<u32, std::vector<u8>>> queue;
    u32 last_tick[kUdsMaxNodes][kUdsMaxNodes + 1] = {};
};

struct LoadResult {
    bool formed = false;
    bool in_order = true;
    u64 room_in_frames = 0, room_in_bytes = 0;   // what the members hand to the room
    u64 room_out_frames = 0, room_out_bytes = 0; // what the room hands to the members
    u64 client_rx_frames = 0, client_rx_bytes = 0; // ... to one client (the last)
    u64 beacons = 0, beacon_bytes = 0;
    u64 prefiltered = 0;
    u32 overwritten = 0; // frames the rings pushed out, all members
    u32 peak_units = 0;
    u32 ring_units = 0;
    u64 pulled = 0;
    u32 frame_wire_bytes = 0;
};

/// `nodes` consoles, each sending one 300-byte broadcast per game frame (60 a second) for `ticks` frames. Every console empties its
/// bind every frame, except that once (from frame 100 on) `gap_ticks` frames go by between two reads (0 or 1 = it never stops).
LoadResult RunLoad(u32 nodes, u32 ticks, u32 gap_ticks) {
    const u32 stall_ticks = gap_ticks > 1 ? gap_ticks - 1 : 0;
    LoadResult r;
    std::memset(&g_stats, 0, sizeof(g_stats));
    LoadNet net(nodes);
    r.formed = net.Form();
    if (!r.formed) {
        return r;
    }
    const u64 base_ms = platform::NowMs();
    for (u32 t = 1; t <= ticks; ++t) {
        for (u32 i = 0; i < nodes; ++i) {
            std::vector<u8> p = Payload('L', static_cast<u8>(i + 1), 0xFF, 0);
            std::memcpy(p.data() + 4, &t, 4);
            net.m[i]->engine->SendTo(kBroadcastNetworkNodeId, kDataChannel, kFrameBytes, 0, p.data(), p.size());
        }
        net.m[0]->engine->Tick(base_ms + static_cast<u64>(t) * 1000 / 60); // (the host's beacons: one every 102.4 ms of this clock)
        net.Pump();
        const bool stalled = stall_ticks != 0 && t >= 100 && t < 100 + stall_ticks;
        for (u32 i = 0; i < nodes && !stalled; ++i) {
            net.Pull(i, 1u << 20, &r.in_order);
        }
    }
    for (u32 i = 0; i < nodes; ++i) {
        const LoadNet::Member& me = *net.m[i];
        r.room_in_frames += me.tx_frames;
        r.room_in_bytes += me.tx_bytes;
        r.room_out_frames += me.rx_frames;
        r.room_out_bytes += me.rx_bytes;
        r.beacons += me.tx_beacons;
        r.beacon_bytes += me.tx_beacon_bytes;
        r.prefiltered += me.prefiltered;
        r.pulled += me.pulled;
    }
    r.client_rx_frames = net.m[nodes - 1]->rx_frames;
    r.client_rx_bytes = net.m[nodes - 1]->rx_bytes;
    r.frame_wire_bytes = kWireHead + static_cast<u32>(GenerateDataPayload(nullptr, 0, 1, 1, 1, 1).size()) + kFrameBytes;
    r.peak_units = g_stats.ring_peak_units;
    for (u32 i = 0; i < nodes; ++i) {
        UdsEngine::UnbindInfo info;
        net.m[i]->engine->Unbind(kBindId, &info);
        r.overwritten += info.overwritten;
    }
    r.ring_units = std::max<u32>(kTetrisRecvBuffer, 0x4000) / RxRing::kUnitBytes;
    return r;
}

/// The shortest time between two reads (in game frames) after which a ring of Tetris's size has pushed a frame out.
u32 ShortestLosingGap(u32 nodes) {
    for (u32 gap = 2; gap <= 64; ++gap) {
        const LoadResult r = RunLoad(nodes, 200, gap);
        if (!r.formed || r.overwritten != 0) {
            return gap;
        }
    }
    return 0;
}

} // namespace

static void TestEightLoad() {
    std::printf("eight nodes: the cost of 8 x 60 broadcasts of 300 bytes a second\n");
    constexpr u32 kSeconds = 10;
    constexpr u32 kTicks = kSeconds * 60;
    const LoadResult eight = RunLoad(8, kTicks, 0);
    const LoadResult four = RunLoad(4, kTicks, 0);
    CHECK(eight.formed && four.formed);
    CHECK(eight.in_order && four.in_order);
    // One frame of 300 bytes is 341 bytes of the room's protocol (19 of the room's header, 8 of LLC, 14 of nwm's, the payload).
    CHECK_EQ(eight.frame_wire_bytes, 341u);
    const u64 data_in8 = eight.room_in_frames - eight.beacons, data_in4 = four.room_in_frames - four.beacons;
    CHECK_EQ(data_in8, u64(8) * kTicks);         // 480 frames a second into the room
    CHECK_EQ(data_in4, u64(4) * kTicks);         // 240
    CHECK_EQ(eight.room_in_bytes - eight.beacon_bytes, u64(8) * kTicks * 341); // 163,680 bytes a second
    CHECK_EQ(eight.room_out_frames, (data_in8 + eight.beacons) * 7);           // each one to the seven others: 3,360 + beacons
    CHECK_EQ(four.room_out_frames, (data_in4 + four.beacons) * 3);
    CHECK_EQ(eight.client_rx_frames - eight.beacons, u64(7) * kTicks);         // one console receives 420 data frames a second
    CHECK_EQ(four.client_rx_frames - four.beacons, u64(3) * kTicks);           // ... 180 with four
    CHECK_EQ(eight.client_rx_bytes, u64(7) * kTicks * 341 + eight.beacon_bytes);
    CHECK(eight.beacons >= 96 && eight.beacons <= 99); // 9.77 a second
    CHECK_EQ(eight.prefiltered, 0u);                   // a broadcast is for everybody: nothing is dropped before the decoder
    // A game that empties its bind every frame loses nothing: the ring never holds more than the seven frames of one round.
    CHECK_EQ(eight.overwritten, 0u);
    CHECK_EQ(four.overwritten, 0u);
    CHECK_EQ(eight.pulled, u64(8) * 7 * kTicks);
    CHECK_EQ(eight.ring_units, 512u);
    CHECK_EQ(eight.peak_units, 7u * RxRing::UnitsFor(kFrameBytes)); // 70 of 512 units
    CHECK_EQ(four.peak_units, 3u * RxRing::UnitsFor(kFrameBytes));  // 30
    // A game that stops reading: the ring holds 51 frames of 300 bytes (512 units, 10 each) and then pushes the OLDEST out. With
    // eight nodes that is 8 game frames between two reads (133 ms), with four it is 18 (300 ms).
    const u32 stall8 = ShortestLosingGap(8), stall4 = ShortestLosingGap(4);
    CHECK_EQ(stall8, 8u);
    CHECK_EQ(stall4, 18u);
    const LoadResult stalled = RunLoad(8, 200, 12); // 200 ms without reading: every console lost frames, and reads the newest whole
    CHECK(stalled.in_order);
    CHECK_EQ(stalled.overwritten, 8u * (12u * 7u - 51u)); // 84 came, 51 fit: 33 lost on each console
    CHECK_EQ(g_stats.ring_overwritten, stalled.overwritten);
    std::printf("  measured: %u s, 300-byte broadcasts at 60 Hz, frame = %u bytes of room protocol\n", kSeconds, eight.frame_wire_bytes);
    for (const LoadResult* r : {&four, &eight}) {
        const u32 n = r == &four ? 4 : 8;
        std::printf("  %u nodes: into the room %llu frames/s %llu B/s | out of the room %llu frames/s %llu B/s | one client receives %llu"
                    " frames/s %llu B/s | beacons %llu (%llu B each) | ring peak %u of %u units, overwritten %u\n",
                    n, static_cast<unsigned long long>(r->room_in_frames / kSeconds),
                    static_cast<unsigned long long>(r->room_in_bytes / kSeconds),
                    static_cast<unsigned long long>(r->room_out_frames / kSeconds),
                    static_cast<unsigned long long>(r->room_out_bytes / kSeconds),
                    static_cast<unsigned long long>(r->client_rx_frames / kSeconds),
                    static_cast<unsigned long long>(r->client_rx_bytes / kSeconds), static_cast<unsigned long long>(r->beacons),
                    static_cast<unsigned long long>(r->beacons ? r->beacon_bytes / r->beacons : 0), r->peak_units, r->ring_units,
                    r->overwritten);
    }
    std::printf("  a game loses its first frame when %u game frames (%u ms) go by between two reads with 8 nodes, %u (%u ms) with 4\n", stall8,
                stall8 * 1000 / 60, stall4, stall4 * 1000 / 60);
}

int main() {
    SetLogSink([](const char*) {}); // quiet
    TestEightNodes();
    TestEightThroughTheHost();
    TestEightAtOnce();
    TestAzaharAskTogether();
    TestLateGoodbyesOfMany();
    TestEightLoad();
    std::printf("\n%d checks, %d failures (%d of the checks hold a known gap as it is today)\n", g_checks, g_failures, g_known_gaps);
    return g_failures ? 1 : 0;
}
