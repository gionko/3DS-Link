// IPC-level test: libctru's REAL UDS/NDM client code (uds.c, ndm.c, compiled unmodified) talks to
// our UdsHle / NdmHle through a fake kernel, and the HLE talks to a REAL azahar-room server.
// The other player is a plain UdsEngine, i.e. the emulator-side behaviour.
//
//   ipc_test <room-host> <room-port>
#include <malloc.h>
#include <arpa/inet.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>

#include <3ds.h>
extern "C" {
#include <3ds/services/ndm.h>
#include <3ds/services/uds.h>
}

#include "azl/log.hpp"
#include "azl/room_client.hpp"
#include "azl/stats.hpp"
#include "azl/uds_engine.hpp"
#include "ctr/kernel_ops.hpp"
#include "ctr/uds_hle.hpp"

#include "../azahar_peer.hpp"
#include "../azahar_rules.hpp"
#include "../support.hpp"

extern azl::ctr::UdsHle* g_test_hle;
extern std::atomic<unsigned> g_fake_friend_seed_low;

using namespace azl;

template <typename Pred>
static bool WaitFor(Pred pred, int timeout_ms = 8000) {
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// A console running the plugin: room client + engine on real kernel-style events + IPC front-end.
struct PluginConsole {
    PluginConsole(const std::string& host, u16 port, const std::string& nick, const std::string& id) {
        config.host = host;
        config.port = port;
        config.nickname = nick;
        config.console_id_hash = id;
        config.game_name = "libctru test";
        UdsEngine::Transport transport;
        transport.send = [this](WifiPacket& p) {
            if (room.joined()) {
                p.transmitter_address = room.mac();
                g_rules.Inspect(p);
                room.SendWifiPacket(p);
            }
        };
        transport.get_mac = [this] { return room.mac(); };
        engine.reset(new UdsEngine(&kernel, transport));
        hle.reset(new ctr::UdsHle(engine.get()));
        room.SetWifiCallback([this](const WifiPacket& p) { engine->OnWifiPacket(p); });
        room.SetTickCallback([this](u64 now_ms) { engine->Tick(now_ms); });
    }
    ~PluginConsole() {
        room.Stop(); // (see Peer: the members declared after `room` are destroyed before it, and the network thread uses them)
    }
    bool Join() {
        room.Start(config);
        const bool joined = WaitFor([this] { return room.joined(); });
        if (joined) {
            g_rules.AddMember(room.mac());
        }
        return joined;
    }
    RoomClient::Config config;
    RoomClient room;
    ctr::CtrKernelOps kernel;
    std::unique_ptr<UdsEngine> engine;
    std::unique_ptr<ctr::UdsHle> hle;
};

// The 5 s ceiling of ConnectToNetwork would make every refused join of the peer a long test.
struct ShortWaitKernel : FakeKernel {
    bool WaitEvent(u32 h, s64 ns) override {
        return FakeKernel::WaitEvent(h, std::min<s64>(ns, 1000000000));
    }
};

// A peer that is just an engine (what an Azahar player is, as far as the protocol goes).
struct Peer {
    Peer(const std::string& host, u16 port, const std::string& nick, const std::string& id) {
        config.host = host;
        config.port = port;
        config.nickname = nick;
        config.console_id_hash = id;
        config.game_name = "emulator peer";
        UdsEngine::Transport transport;
        transport.send = [this](WifiPacket& p) {
            if (room.joined()) {
                p.transmitter_address = room.mac();
                g_rules.Inspect(p);
                room.SendWifiPacket(p);
            }
        };
        transport.get_mac = [this] { return room.mac(); };
        engine.reset(new UdsEngine(&kernel, transport));
        room.SetWifiCallback([this](const WifiPacket& p) { engine->OnWifiPacket(p); });
        room.SetTickCallback([this](u64 now_ms) { engine->Tick(now_ms); });
    }
    ~Peer() {
        room.Stop(); // (the network thread calls into the engine, and the members declared after `room` are destroyed before it)
    }
    bool Join() {
        room.Start(config);
        const bool joined = WaitFor([this] { return room.joined(); });
        if (joined) {
            g_rules.AddMember(room.mac());
        }
        return joined;
    }
    RoomClient::Config config;
    RoomClient room;
    ShortWaitKernel kernel;
    std::unique_ptr<UdsEngine> engine;
};

static std::string Username(const udsNodeInfo& node) {
    char name[16] = {};
    udsGetNodeInfoUsername(&node, name);
    return name;
}

// ---------------------------------------------------------------------------------------------
extern "C" Handle ndmuHandle; // (libctru's ndm.c: the session the client calls through)

static void TestNdmThroughHle(PluginConsole& console) {
    std::printf("libctru ndm client -> NdmHle\n");
    // The firmware's codes (ndm module 02.46.01, see ndm_hle.hpp).
    constexpr u32 kAlreadyInState = 0xC8A06811, kNotLocked = 0xC8A0680B, kBadArgument = 0xD8E06BED;
    constexpr u32 kNotSuspended = 0xC8A0680D, kBadState = 0xC8A0680E;
    // libctru writes the PID descriptor and leaves the word after it as the command buffer held it (the
    // kernel fills it in, and the hook runs before the kernel): poison it, the answers must not depend on it.
    auto poison = [](unsigned word, u32 value) { getThreadCommandBuffer()[word] = value; };

    CHECK_EQ(ndmuInit(), 0);
    ndmExclusiveState state = NDM_EXCLUSIVE_STATE_NONE;
    poison(3, 0xDEADBEEF);
    CHECK_EQ(NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_LOCAL_COMMUNICATIONS), 0);
    CHECK_EQ(NDMU_GetExclusiveState(&state), 0);
    CHECK_EQ(static_cast<int>(state), static_cast<int>(NDM_EXCLUSIVE_STATE_LOCAL_COMMUNICATIONS));
    poison(3, 0x12345678);
    CHECK_EQ(NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_LOCAL_COMMUNICATIONS), static_cast<int>(kAlreadyInState));
    poison(2, 0x0BADC0DE);
    CHECK_EQ(NDMU_LeaveExclusiveState(), 0);
    CHECK_EQ(NDMU_GetExclusiveState(&state), 0);
    CHECK_EQ(static_cast<int>(state), static_cast<int>(NDM_EXCLUSIVE_STATE_NONE));
    CHECK_EQ(NDMU_LeaveExclusiveState(), 0); // nobody owns the state: nothing to do

    // the state lock
    CHECK_EQ(NDMU_UnlockState(), static_cast<int>(kNotLocked));
    CHECK_EQ(NDMU_LockState(), 0);
    CHECK_EQ(NDMU_LockState(), 0);
    CHECK_EQ(NDMU_UnlockState(), 0);
    CHECK_EQ(NDMU_UnlockState(), 0);
    CHECK_EQ(NDMU_UnlockState(), static_cast<int>(kNotLocked));

    // the daemons: mask range, counts per session, the default (Boss and Nim are suspended by it)
    CHECK_EQ(NDMU_SuspendDaemons(static_cast<ndmDaemonMask>(0)), static_cast<int>(kBadArgument));
    CHECK_EQ(NDMU_SuspendDaemons(static_cast<ndmDaemonMask>(16)), static_cast<int>(kBadArgument));
    CHECK_EQ(NDMU_ResumeDaemons(NDM_DAEMON_MASK_CEC), static_cast<int>(kNotSuspended));
    CHECK_EQ(NDMU_SuspendDaemons(static_cast<ndmDaemonMask>(NDM_DAEMON_MASK_CEC | NDM_DAEMON_MASK_FRIENDS)), 0);
    ndmDaemonStatus status = NDM_DAEMON_STATUS_BUSY;
    CHECK_EQ(NDMU_QueryStatus(NDM_DAEMON_CEC, &status), 0);
    CHECK_EQ(static_cast<int>(status), static_cast<int>(NDM_DAEMON_STATUS_SUSPENDED));
    CHECK_EQ(NDMU_ResumeDaemons(static_cast<ndmDaemonMask>(NDM_DAEMON_MASK_CEC | NDM_DAEMON_MASK_FRIENDS)), 0);
    CHECK_EQ(NDMU_QueryStatus(NDM_DAEMON_CEC, &status), 0);
    CHECK_EQ(static_cast<int>(status), static_cast<int>(NDM_DAEMON_STATUS_IDLE));
    CHECK_EQ(NDMU_QueryStatus(NDM_DAEMON_BOSS, &status), 0);
    CHECK_EQ(static_cast<int>(status), static_cast<int>(NDM_DAEMON_STATUS_SUSPENDED)); // the default mask 0x09
    CHECK_EQ(NDMU_QueryStatus(static_cast<ndmDaemon>(5), &status), static_cast<int>(kBadArgument));
    ndmDaemonMask defaults = static_cast<ndmDaemonMask>(0);
    CHECK_EQ(NDMU_GetDefaultDaemons(&defaults), 0);
    CHECK_EQ(static_cast<int>(defaults), static_cast<int>(NDM_DAEMON_MASK_DEFAULT));
    CHECK_EQ(NDMU_ResetDaemons(), static_cast<int>(kBadState)); // only the session that overrode them may reset

    // the scheduler and the intervals
    CHECK_EQ(NDMU_ResumeScheduler(), static_cast<int>(kNotSuspended));
    CHECK_EQ(NDMU_SuspendScheduler(0), 0);
    CHECK_EQ(NDMU_ResumeScheduler(), 0);
    u32 scan = 0, retry = 0;
    CHECK_EQ(NDMU_GetScanInterval(&scan), 0);
    CHECK_EQ(NDMU_GetRetryInterval(&retry), 0);
    CHECK_EQ(scan, 30u);
    CHECK_EQ(retry, 10u);
    CHECK_EQ(NDMU_SetScanInterval(77), 0);
    CHECK_EQ(NDMU_GetScanInterval(&scan), 0);
    CHECK_EQ(scan, 77u);
    ndmState current = static_cast<ndmState>(99);
    CHECK_EQ(NDMU_GetCurrentState(&current), 0); // (what the value means is not known: only that it answers)
    CHECK_EQ(NDMU_ClearMacFilter(), 0);

    // the game holds the state when its session goes away: the session's close hands everything back
    CHECK_EQ(NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_LOCAL_COMMUNICATIONS), 0);
    CHECK_EQ(NDMU_LockState(), 0);
    console.hle->NoteHandleClosed(ndmuHandle); // (the game's svcCloseHandle hook)
    ndmuExit();
    CHECK_EQ(ndmuInit(), 0);
    CHECK_EQ(NDMU_GetExclusiveState(&state), 0);
    CHECK_EQ(static_cast<int>(state), static_cast<int>(NDM_EXCLUSIVE_STATE_NONE));
    CHECK_EQ(NDMU_GetScanInterval(&scan), 0);
    CHECK_EQ(scan, 30u);
    CHECK_EQ(NDMU_UnlockState(), static_cast<int>(kNotLocked));
    CHECK_EQ(console.hle->ndm_header_anomalies(), 0u); // libctru's headers are the firmware's
    console.hle->NoteHandleClosed(ndmuHandle);
    ndmuExit();
}

static void TestLibctruHosts(PluginConsole& a, Peer& b) {
    std::printf("libctru UDS client hosts a network, emulator peer joins\n");

    CHECK_EQ(udsInit(0x3000, "Hosty"), 0);
    CHECK(a.hle->sessions() >= 1u);

    udsNetworkStruct net;
    udsGenerateDefaultNetworkStruct(&net, 0x00112233, 0x01, 4);
    udsBindContext bind;
    CHECK_EQ(udsCreateNetwork(&net, "pass", 5, &bind, 1, UDS_DEFAULT_RECVBUFSIZE), 0);

    const char app[] = "lobby: 3DS hosting";
    CHECK_EQ(udsSetApplicationData(app, sizeof(app)), 0);
    char app_back[0x20] = {};
    size_t app_size = 0;
    CHECK_EQ(udsGetApplicationData(app_back, sizeof(app_back), &app_size), 0);
    CHECK_EQ(app_size, sizeof(app));
    CHECK(std::strcmp(app_back, app) == 0);

    udsConnectionStatus st;
    CHECK_EQ(udsGetConnectionStatus(&st), 0);
    CHECK_EQ(st.status, 6u); // ConnectedAsHost
    CHECK_EQ(st.cur_NetworkNodeID, 1);
    CHECK_EQ(st.total_nodes, 1);
    CHECK_EQ(st.max_nodes, 4);
    CHECK_EQ(st.unk_x4, 1u); // reason NETWORK_IS_AVAILABLE
    u8 channel = 0;
    CHECK_EQ(udsGetChannel(&channel), 0);
    CHECK_EQ(channel, 11);

    // The emulator peer finds the beacon the 3DS broadcast through the room and joins
    u32 peer_event = 0;
    CHECK_EQ(b.engine->Initialize(MakeNode(0xBEEF, "Peer", 0), kTestSharedMemory, kTestVersion, &peer_event), 0u);
    std::vector<u8> scan(0x4000);
    u32 entries = 0;
    CHECK(WaitFor([&] {
        b.engine->RecvBeaconBroadcastData(scan.data(), static_cast<u32>(scan.size()), kBroadcastMac);
        entries = LoadLE32(scan.data() + 8);
        return entries > 0;
    }));
    CHECK_EQ(entries, 1u);
    const u8* entry = scan.data() + kBeaconDataReplyHeaderSize;
    const u32 entry_size = LoadLE32(entry);
    std::vector<u8> beacon(entry + kBeaconEntryHeaderSize, entry + entry_size);
    NetworkInfo scanned;
    std::memset(&scanned, 0, sizeof(scanned));
    const u8* net_tag = nullptr;
    const u8 *tag0 = nullptr, *tag1 = nullptr;
    std::size_t tag0_len = 0, tag1_len = 0;
    for (std::size_t pos = 12; pos + 2 <= beacon.size();) {
        const u8 id = beacon[pos], len = beacon[pos + 1];
        pos += 2;
        if (id == 0xdd && std::memcmp(&beacon[pos], kNintendoOui, 3) == 0) {
            const u8 type = beacon[pos + 3];
            if (type == 21) {
                net_tag = &beacon[pos];
            } else if (type == 24) {
                tag0 = &beacon[pos];
                tag0_len = len;
            } else if (type == 25) {
                tag1 = &beacon[pos];
                tag1_len = len;
            }
        }
        pos += len;
    }
    CHECK(net_tag && tag0);
    std::memcpy(scanned.oui_value, net_tag, 0x1F);
    scanned.channel = entry[5];
    std::memcpy(scanned.host_mac_address.data(), entry + 8, 6);
    CHECK_EQ(scanned.wlan_comm_id(), 0x00112233u);

    // The node list inside that beacon must decrypt to the 3DS host's node info
    {
        u8 t0[0xFE] = {}, t1[0xFE] = {};
        std::memcpy(t0, tag0, tag0_len);
        if (tag1) {
            std::memcpy(t1, tag1, tag1_len);
        }
        std::vector<u8> nodes;
        CHECK_EQ(b.engine->DecryptBeaconData(scanned, t0, sizeof(t0), t1, sizeof(t1), &nodes), 0u);
        NodeInfo first;
        first.Load(nodes.data());
        CHECK_EQ(first.username[0], u16('H'));
        CHECK_EQ(first.network_node_id, 1);
        CHECK_EQ(first.friend_code_seed, u64(0x0000000111111111ULL));
    }

    u32 peer_bind = 0;
    CHECK_EQ(b.engine->Bind(2, 0x5F4, 1, kBroadcastNetworkNodeId, &peer_bind), 0u);
    std::thread joiner([&] {
        CHECK_EQ(b.engine->ConnectToNetwork(reinterpret_cast<u8*>(&scanned), sizeof(scanned), 1), 0u);
    });

    // The 3DS side learns about the new node through the connection status event
    CHECK(WaitFor([&] {
        udsGetConnectionStatus(&st);
        return st.total_nodes == 2;
    }));
    joiner.join();
    CHECK_EQ(st.node_bitmask, 3);
    udsNodeInfo node;
    CHECK_EQ(udsGetNodeInformation(2, &node), 0);
    CHECK(Username(node) == "Peer");
    CHECK_EQ(node.uds_friendcodeseed, u64(0xBEEF));

    // Data both ways
    const u8 hello[] = "hello from the 3DS";
    CHECK_EQ(udsSendTo(2, 1, UDS_SENDFLAG_Default, hello, sizeof(hello)), 0);
    std::vector<u8> out;
    u32 size = 0;
    u16 src = 0;
    CHECK(WaitFor([&] {
        PullInto(*b.engine, 2, &out, &size, &src);
        return size != 0;
    }));
    CHECK_EQ(size, u32(sizeof(hello)));
    CHECK_EQ(src, 1);
    CHECK(std::memcmp(out.data(), hello, sizeof(hello)) == 0);

    const u8 reply[] = "reply from the emulator";
    CHECK_EQ(b.engine->SendTo(1, 1, sizeof(reply), 0, reply, sizeof(reply)), 0u);
    CHECK(udsWaitDataAvailable(&bind, false, false) || WaitFor([&] { return udsWaitDataAvailable(&bind, false, false); }));
    char rx[0x600] = {};
    size_t actual = 0;
    u16 from = 0;
    CHECK_EQ(udsPullPacket(&bind, rx, sizeof(rx), &actual, &from), 0);
    CHECK_EQ(actual, sizeof(reply));
    CHECK_EQ(from, 2);
    CHECK(std::memcmp(rx, reply, sizeof(reply)) == 0);

    // Nothing left: PullPacket reports zero bytes and leaves the game's buffer alone
    std::memset(rx, 0xA5, sizeof(rx));
    CHECK_EQ(udsPullPacket(&bind, rx, sizeof(rx), &actual, &from), 0);
    CHECK_EQ(actual, std::size_t(0));
    char untouched[sizeof(rx)];
    std::memset(untouched, 0xA5, sizeof(untouched));
    CHECK(std::memcmp(rx, untouched, sizeof(rx)) == 0);

    // Peer leaves; the host notices
    CHECK_EQ(b.engine->DisconnectNetwork(), 0u);
    CHECK(WaitFor([&] {
        udsGetConnectionStatus(&st);
        return st.total_nodes == 1;
    }));

    // The host closes the lobby (udsSetNewConnectionsBlocked): the beacon says so, a new join gets no
    // answer, the node that is in stays; opening it again lets the join through
    CHECK_EQ(udsSetNewConnectionsBlocked(true, true, false), 0);
    NetworkInfo closed;
    CHECK(WaitFor([&] {
        b.engine->RecvBeaconBroadcastData(scan.data(), static_cast<u32>(scan.size()), kBroadcastMac);
        return LoadLE32(scan.data() + 8) > 0 && FirstScannedNetwork(scan, &closed) && (LoadBE16(closed.attributes_be) & 2);
    }));
    CHECK_EQ(b.engine->ConnectToNetwork(reinterpret_cast<u8*>(&closed), sizeof(closed), 1), res::kConnectTimeout);
    CHECK_EQ(udsGetConnectionStatus(&st), 0);
    CHECK_EQ(st.total_nodes, 1);
    CHECK_EQ(udsSetNewConnectionsBlocked(false, true, false), 0);
    NetworkInfo open;
    CHECK(WaitFor([&] {
        b.engine->RecvBeaconBroadcastData(scan.data(), static_cast<u32>(scan.size()), kBroadcastMac);
        return LoadLE32(scan.data() + 8) > 0 && FirstScannedNetwork(scan, &open) && !(LoadBE16(open.attributes_be) & 2);
    }));
    CHECK_EQ(b.engine->ConnectToNetwork(reinterpret_cast<u8*>(&open), sizeof(open), 1), 0u);
    CHECK(WaitFor([&] {
        udsGetConnectionStatus(&st);
        return st.total_nodes == 2;
    }));
    // EjectClient: the host drops the node itself (an Azahar client would ignore the Deauthentication)
    CHECK_EQ(udsEjectClient(2), 0);
    CHECK_EQ(udsGetConnectionStatus(&st), 0);
    CHECK_EQ(st.total_nodes, 1);
    CHECK_EQ(st.node_bitmask, 1);
    CHECK_EQ(udsEjectClient(1), static_cast<int>(0xE10113EA)); // the host itself
    CHECK_EQ(b.engine->DisconnectNetwork(), 0u);

    CHECK_EQ(udsDestroyNetwork(), 0);
    CHECK_EQ(udsGetConnectionStatus(&st), 0);
    CHECK_EQ(st.status, 3u);
    CHECK_EQ(st.unk_x4, 2u); // REQUEST_FROM_MYSELF
    CHECK_EQ(udsDestroyNetwork(), static_cast<int>(0xC8A113EA));
    CHECK_EQ(udsUnbind(&bind), 0);
    udsExit();
    b.engine->Shutdown();
    CHECK_RULES();
}

static void TestLibctruJoins(PluginConsole& a, Peer& b) {
    std::printf("emulator peer hosts, libctru UDS client scans and joins\n");

    u32 ev;
    CHECK_EQ(b.engine->Initialize(MakeNode(0xFEED, "EmuHost", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    NetworkInfo ni = MakeNetworkInfo(4, 5);
    StoreBE32(ni.wlan_comm_id_be, 0x00112233);
    ni.id = 1;
    ni.application_data[0] = 'E';
    ni.application_data[1] = 'M';
    ni.application_data[2] = 'U';
    ni.application_data[3] = 0;
    ni.application_data_size = 4;
    CHECK_EQ(b.engine->BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni)), 0u);
    u32 peer_bind = 0;
    CHECK_EQ(b.engine->Bind(2, 0x5F4, 1, kBroadcastNetworkNodeId, &peer_bind), 0u);

    CHECK_EQ(udsInit(0x3000, "Joiner"), 0);

    // udsScanBeacons parses the beacon with libctru's own code and asks DecryptBeaconData
    std::vector<u8> scan_buffer(0x4000);
    udsNetworkScanInfo* networks = nullptr;
    size_t total = 0;
    CHECK(WaitFor([&] {
        if (networks) {
            free(networks);
            networks = nullptr;
        }
        const Result rc = udsScanBeacons(scan_buffer.data(), scan_buffer.size(), &networks, &total,
                                         0x00112233, 0x01, nullptr, false);
        return rc == 0 && total >= 1;
    }, 15000));
    CHECK_EQ(total, std::size_t(1));
    if (total < 1) {
        return;
    }
    {
        // libctru's own scan listens for 3 channels x 110 ms and, as nothing is consumed, finds the same
        // host again at once
        udsNetworkScanInfo* again = nullptr;
        size_t again_total = 0;
        const auto t0 = std::chrono::steady_clock::now();
        const Result rc = udsScanBeacons(scan_buffer.data(), scan_buffer.size(), &again, &again_total, 0x00112233,
                                         0x01, nullptr, false);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK_EQ(rc, 0);
        CHECK_EQ(again_total, std::size_t(1));
        CHECK(ms >= 325 && ms <= 700);
        free(again);
    }
    CHECK(Username(networks[0].nodes[0]) == "EmuHost");
    CHECK_EQ(networks[0].nodes[0].uds_friendcodeseed, u64(0xFEED));
    CHECK_EQ(ntohl(networks[0].network.wlancommID), 0x00112233u);
    CHECK_EQ(networks[0].network.max_nodes, 4);
    char app[8] = {};
    size_t app_size = 0;
    CHECK_EQ(udsGetNetworkStructApplicationData(&networks[0].network, app, sizeof(app), &app_size), 0);
    CHECK_EQ(app_size, std::size_t(4));
    CHECK(std::strcmp(app, "EMU") == 0);

    udsBindContext bind;
    CHECK_EQ(udsConnectNetwork(&networks[0].network, "pass", 5, &bind, UDS_BROADCAST_NETWORKNODEID,
                               UDSCONTYPE_Client, 1, UDS_DEFAULT_RECVBUFSIZE),
             0);
    udsConnectionStatus st;
    CHECK_EQ(udsGetConnectionStatus(&st), 0);
    CHECK_EQ(st.status, 9u); // ConnectedAsClient
    CHECK_EQ(st.cur_NetworkNodeID, 2);
    CHECK_EQ(st.total_nodes, 2);
    CHECK_EQ(st.node_bitmask, 3);
    udsNodeInfo host_node;
    CHECK_EQ(udsGetNodeInformation(1, &host_node), 0);
    CHECK(Username(host_node) == "EmuHost");

    // client -> host
    const u8 msg[] = "joiner says hi";
    CHECK_EQ(udsSendTo(UDS_HOST_NETWORKNODEID, 1, UDS_SENDFLAG_Default, msg, sizeof(msg)), 0);
    std::vector<u8> out;
    u32 size = 0;
    u16 src = 0;
    CHECK(WaitFor([&] {
        PullInto(*b.engine, 2, &out, &size, &src);
        return size != 0;
    }));
    CHECK_EQ(size, u32(sizeof(msg)));
    CHECK_EQ(src, 2);
    CHECK(std::memcmp(out.data(), msg, sizeof(msg)) == 0);

    // host -> client, largest frame
    std::vector<u8> big(UDS_DATAFRAME_MAXSIZE);
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<u8>(i * 3 + 1);
    }
    CHECK_EQ(b.engine->SendTo(2, 1, static_cast<u32>(big.size()), 0, big.data(), big.size()), 0u);
    CHECK(WaitFor([&] { return udsWaitDataAvailable(&bind, false, false); }));
    std::vector<u8> rx(0x600);
    size_t actual = 0;
    u16 from = 0;
    CHECK_EQ(udsPullPacket(&bind, rx.data(), rx.size(), &actual, &from), 0);
    CHECK_EQ(actual, big.size());
    CHECK_EQ(from, 1);
    CHECK(std::memcmp(rx.data(), big.data(), big.size()) == 0);

    // the 3DS leaves: reason REQUEST_FROM_MYSELF, and the emulator host notices
    CHECK_EQ(udsDisconnectNetwork(), 0);
    CHECK_EQ(udsGetConnectionStatus(&st), 0);
    CHECK_EQ(st.status, 3u); // NotConnected
    CHECK_EQ(st.unk_x4, 2u);
    CHECK(WaitFor([&] { return b.engine->GetConnectionStatus().total_nodes == 1; }));

    CHECK_EQ(udsUnbind(&bind), 0);
    CHECK_EQ(b.engine->DestroyNetwork(), 0u);

    // libctru's udsConnectNetwork to a network that is gone: the answer is the timeout, then it
    // calls udsDisconnectNetwork itself, which is silent (no session): the game ends up idle, reason 0
    udsBindContext bind2;
    const Result gone = udsConnectNetwork(&networks[0].network, "pass", 5, &bind2, UDS_BROADCAST_NETWORKNODEID,
                                          UDSCONTYPE_Client, 1, UDS_DEFAULT_RECVBUFSIZE);
    CHECK_EQ(gone, static_cast<Result>(0xC92113FE));
    CHECK_EQ(udsGetConnectionStatus(&st), 0);
    CHECK_EQ(st.status, 3u);
    CHECK_EQ(st.unk_x4, 0u);
    udsExit();
    free(networks);
    CHECK_RULES();
    (void)a;
}

// The room link is lost while libctru's UDS client is in a session (spec I1): the game sees what a console's radio
// failing shows it. Its status event is signalled, the status reads idle with the reason "connection lost", a thread
// that waits for data wakes up, the data commands answer "wrong state", the cleanup libctru does is silent, and the
// game can join again at once.
static void TestLibctruRoomLoss(PluginConsole& a, Peer& b) {
    std::printf("the room link is lost under libctru's UDS client\n");
    NetworkInfo ni = MakeNetworkInfo(4, 5);
    StoreBE32(ni.wlan_comm_id_be, 0x00112233);
    ni.id = 1;
    CHECK_EQ(b.engine->BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni)), 0u);
    // (a block big enough for the two binds this test makes: the game's block is what a bind is charged to)
    CHECK_EQ(udsInit(0x8000, "Joiner"), 0);

    std::vector<u8> scan_buffer(0x4000);
    udsNetworkScanInfo* networks = nullptr;
    size_t total = 0;
    CHECK(WaitFor([&] {
        if (networks) {
            free(networks);
            networks = nullptr;
        }
        const Result rc = udsScanBeacons(scan_buffer.data(), scan_buffer.size(), &networks, &total, 0x00112233, 0x01,
                                         nullptr, false);
        return rc == 0 && total >= 1;
    }, 15000));
    if (total < 1) {
        return;
    }
    udsBindContext bind;
    CHECK_EQ(udsConnectNetwork(&networks[0].network, "pass", 5, &bind, UDS_BROADCAST_NETWORKNODEID, UDSCONTYPE_Client, 1,
                               UDS_DEFAULT_RECVBUFSIZE),
             0);
    udsConnectionStatus st;
    CHECK_EQ(udsGetConnectionStatus(&st), 0);
    CHECK_EQ(st.status, 9u);
    while (udsWaitConnectionStatusEvent(false, false)) {
    } // (the events of the join)

    // a game thread waits for data on its bind, like FFE's event thread
    std::atomic<int> woke{0};
    std::thread waiter([&] {
        if (udsWaitDataAvailable(&bind, false, true)) {
            woke = 1;
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_EQ(woke.load(), 0);

    const u32 down = g_stats.transport_down[0];
    CHECK(a.engine->OnTransportDown(NetworkStatusChangeReason::ConnectionLost));
    waiter.join();
    CHECK_EQ(woke.load(), 1); // the bind's event was signalled
    CHECK(udsWaitConnectionStatusEvent(false, false)); // ... and the status event
    CHECK(!udsWaitConnectionStatusEvent(false, false)); // (once)
    CHECK_EQ(udsGetConnectionStatus(&st), 0);
    CHECK_EQ(st.status, 3u);
    CHECK_EQ(st.unk_x4, 5u); // CONNECTION_LOST
    CHECK_EQ(st.cur_NetworkNodeID, 2);
    CHECK_EQ(st.total_nodes, 0);
    std::vector<u8> rx(0x600);
    size_t actual = 0;
    u16 from = 0;
    CHECK_EQ(udsPullPacket(&bind, rx.data(), rx.size(), &actual, &from), static_cast<int>(0xC8A113EA));
    const u8 msg[] = "nobody is there";
    CHECK_EQ(udsSendTo(UDS_HOST_NETWORKNODEID, 1, UDS_SENDFLAG_Default, msg, sizeof(msg)), static_cast<int>(0xC8A113EA));
    CHECK(!a.engine->OnTransportDown(NetworkStatusChangeReason::ConnectionLost)); // a second call changes nothing
    CHECK(!udsWaitConnectionStatusEvent(false, false));
    CHECK_EQ(g_stats.transport_down[0] - down, 1u);

    // the cleanup libctru does after a failure is silent, and the game joins again at once (no udsInit): the host
    // still has the old node, the new Authentication replaces it
    CHECK_EQ(udsDisconnectNetwork(), 0);
    udsBindContext bind2;
    CHECK_EQ(udsConnectNetwork(&networks[0].network, "pass", 5, &bind2, UDS_BROADCAST_NETWORKNODEID, UDSCONTYPE_Client, 2,
                               UDS_DEFAULT_RECVBUFSIZE),
             0);
    CHECK_EQ(udsGetConnectionStatus(&st), 0);
    CHECK_EQ(st.status, 9u);
    CHECK_EQ(st.total_nodes, 2);
    // the bind the game made before the loss (channel 1) survived it and takes the host's next frame
    const u8 hello[] = "hello again";
    CHECK_EQ(b.engine->SendTo(0xFFFF, 1, sizeof(hello), 0, hello, sizeof(hello)), 0u);
    CHECK(WaitFor([&] { return udsWaitDataAvailable(&bind, false, false); }));
    CHECK_EQ(udsPullPacket(&bind, rx.data(), rx.size(), &actual, &from), 0);
    CHECK_EQ(actual, sizeof(hello));
    CHECK_EQ(from, 1);

    CHECK_EQ(udsDisconnectNetwork(), 0);
    CHECK_EQ(udsUnbind(&bind), 0);
    CHECK_EQ(udsUnbind(&bind2), 0);
    CHECK_EQ(b.engine->DestroyNetwork(), 0u);
    udsExit();
    free(networks);
    CHECK_RULES();
    (void)a;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s room-host room-port\n", argv[0]);
        return 2;
    }
    SetLogSink([](const char* line) { std::fprintf(stderr, "  [log] %s\n", line); });
    const std::string host = argv[1];
    const u16 port = static_cast<u16>(std::atoi(argv[2]));

    PluginConsole console(host, port, "Console3DS", "3ds-console");
    Peer peer(host, port, "EmuPlayer", "emu-peer");
    CHECK(console.Join());
    CHECK(peer.Join());
    g_test_hle = console.hle.get();

    TestNdmThroughHle(console);
    TestLibctruHosts(console, peer);
    TestLibctruJoins(console, peer);
    TestLibctruRoomLoss(console, peer);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
