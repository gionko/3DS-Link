// Golden replay of a real FFE session (spec C9): the request words of Final Fantasy Explorers as the
// debug captures of hardware tests #9-#11 recorded them
// go through UdsHle::Dispatch (via the fake srv:GetServiceHandle, exactly like the hooked svc) into a
// 3DS console that is a CLIENT of an Azahar host with two more emulator players around it. The host and the two players
// are the frozen Azahar models of azahar_peer.hpp (AzaharHost, AzaharClient), not our own engine: what the 3DS is tested
// against does not change when the engine does.
//
// What the replay pins down, in the order FFE does it:
//   ndm:u   Override(0xF), Suspend(6), Enter(2) ... Leave: result 0 everywhere; when the session closes
//           the default mask is 0x09 again and Boss and Nim are suspended once (the real ndm's state)
//   nwm::UDS  Initialize 0x1B (version 0x400, shared memory 0x14000)  -> reply [res 0, desc 0, event]
//             GetConnectionStatus                                    -> [3, 0, 0]
//             RecvBeaconBroadcastData (twice, 330 ms each)           -> the Azahar host, one entry
//                                     (and once with its cancel event signalled: ends at once)
//             Bind(2, 0x2E30, ch 0xF3, src 0xFFFF)
//             ConnectToNetwork type 1                                -> 0; status [9, 1, changed<<16 | node]
//             GetApplicationData(0xC9) twice                         -> 0x94 bytes
//             a further player joining                               -> status event, changed bit
//             PullPacket / SendTo streams with the FFE flags (broadcast MAC, one frame per destination)
//             exit: Unbind(2), Bind(4), DisconnectNetwork, Shutdown, both sessions closed, ndm Leave
//   and what the game is told through events: the status event is signalled exactly at Connect, at the
//   further join and at Disconnect; a bind event exactly once per frame delivered to that bind (and
//   once more when the bind goes away; the one that is left is woken at Disconnect and again when
//   Shutdown frees it).
// It does NOT assert an order between the game's event thread (GetConnectionStatus) and
// ConnectToNetwork returning: in the captures it is 1 ms one way and 77 ms the other.
//
// Debug capture (spec C14): the same run checks that every command line carries all the words its
// header declares.
//
//   ffe_replay_test            (needs no room server; built by Tests/ipc_test/build.sh)
#include <chrono>
#include <cstdlib>
#include <set>
#include <thread>

#include "azl/log.hpp"

#include "hle_harness.hpp"

using namespace azl;

// The main log (AzaharLink.log), as the tests see it.
static std::vector<std::string> g_log;
static std::mutex g_log_mutex;
static int LogLines(const std::string& prefix) {
    std::lock_guard<std::mutex> l(g_log_mutex);
    int n = 0;
    for (const std::string& line : g_log) {
        if (line.compare(0, prefix.size(), prefix) == 0) {
            ++n;
        }
    }
    return n;
}

// The main-log lines of one UDS command, without the once-per-process "is first called by thread" line that
// NoteCommand adds to the first call of each command (tappa O).
static int CommandLines(const std::string& id) {
    const std::string prefix = "uds: command " + id + " ";
    return LogLines(prefix) - LogLines(prefix + "is first called by thread ");
}

static const u32 kFfeWlanCommId = 0x0016ED10;
static const u8 kFfeNetworkId8 = 1;
static const u8 kChannel = 0xF3; // FFE's data channel, 243
static const u32 kBindSize = 0x2E30;

static std::vector<u8> Payload(u8 src, u8 seq, std::size_t size) {
    std::vector<u8> v(size);
    for (std::size_t i = 0; i < size; ++i) {
        v[i] = static_cast<u8>(src * 31 + seq * 7 + i);
    }
    v[0] = src;
    v[1] = seq;
    return v;
}

/// Beacons of the Azahar host, every 100 ms, like its timer.
struct Beaconer {
    explicit Beaconer(AzaharHost& h) : host(h), thread([this] {
        while (!stop.load()) {
            host.SendBeacon();
            for (int i = 0; i < 10 && !stop.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }) {}
    ~Beaconer() {
        stop = true;
        thread.join();
    }
    AzaharHost& host;
    std::atomic<bool> stop{false};
    std::thread thread;
};

// The request must carry exactly the words the capture logged (cmd[1..n]).
static void ExpectGolden(const std::vector<u32>& request, std::initializer_list<u32> golden) {
    std::size_t i = 1;
    for (u32 w : golden) {
        ++g_checks;
        if (i >= request.size() || request[i] != w) {
            ++g_failures;
            std::fprintf(stderr, "  FAIL request 0x%08lx word %zu: 0x%08lx, the capture has 0x%08lx\n",
                         static_cast<unsigned long>(request[0]), i,
                         i < request.size() ? static_cast<unsigned long>(request[i]) : 0UL,
                         static_cast<unsigned long>(w));
        }
        ++i;
    }
}

static void ExpectWords(DebugLines& debug, const char* prefix, int request_words, int reply_words) {
    const std::string line = debug.Last(prefix);
    const auto counts = DebugLines::WordCounts(line);
    ++g_checks;
    if (counts.first != request_words || counts.second != reply_words) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL debug line '%s': %d request / %d reply words, expected %d / %d\n    %s\n", prefix,
                     counts.first, counts.second, request_words, reply_words, line.c_str());
    }
}

// `foreign_lobby`: the client's foreign-lobby filter is on and a stranger from another lobby of the room sends frames with the
// node ids of this one all along (unicast to the 3DS: an Azahar host re-broadcasts what is meant for another node, and that
// copy would carry the host's own MAC). Nothing of the replay may change - every check below is the same - and not one
// of the strangers' frames may reach the game. Without it (the default of the plugin) the filter only counts, and a real
// session must not have counted one.
static void TestFfeReplay(bool foreign_lobby) {
    std::printf("FFE golden replay: 3DS client of an Azahar host, two emulator players around it%s\n",
                foreign_lobby ? " (foreign-lobby filter on, a stranger in the room)" : "");
    g_rules.Reset();
    std::memset(&g_stats, 0, sizeof(g_stats));
    {
        std::lock_guard<std::mutex> l(g_log_mutex);
        g_log.clear(); // (the checks on the main log count lines: this run's)
    }
    DebugLines debug;
    MockRoom room;

    NetworkInfo net = MakeNetworkInfo(4, 148); // 0x94 bytes of application data, as in the captures
    StoreBE32(net.wlan_comm_id_be, kFfeWlanCommId);
    net.id = kFfeNetworkId8;
    AzaharHost host(room, 1, net, MakeNode(0x0000000100000001ULL, "Friend-A", 0));
    AzaharClient peer(room, 2, MakeNode(0x2222, "Friend-F", 0));       // joins first: node 2
    AzaharClient latecomer(room, 4, MakeNode(0x4444, "Friend-C", 0)); // joins after the 3DS: node 4
    peer.Bind(kChannel);
    latecomer.Bind(kChannel);
    HleConsole ds(room, 3);  // the 3DS: node 3
    Beaconer beacons(host);
    const Mac stranger_mac{0x00, 0x1F, 0x32, 0x00, 0x00, 0x66};
    const int stranger = room.AddMember(stranger_mac, [](const WifiPacket&) {});
    if (foreign_lobby) {
        ds.console.engine->SetClientForeignFilterEnforced(true);
    }
    u32 strangers_sent = 0;
    auto strangers_talk = [&](u8 seq) {
        // the other lobby's host (node 1) to the 3DS's node, one of its clients (node 2) to everybody, and another (node 4)
        // to the 3DS's node
        struct Foreign {
            u16 src, dest;
        };
        for (const Foreign f : {Foreign{1, 3}, Foreign{2, kBroadcastNetworkNodeId}, Foreign{4, 3}}) {
            const std::vector<u8> body = {0xF0, seq, static_cast<u8>(f.src), 'x', 'y', 'z'};
            WifiPacket p;
            p.type = WifiPacket::Type::Data;
            p.channel = 11;
            p.transmitter_address = stranger_mac;
            p.destination_address = ds.console.mac;
            p.data = GenerateDataPayload(body.data(), body.size(), kChannel, f.dest, f.src, 0);
            room.Send(stranger, p);
            ++strangers_sent;
        }
    };
    CountingKernel& kernel = ds.kernel;

    // ---- the emulator player that is in the lobby already ---------------------------------------
    NetworkInfo target = host.network();
    peer.Connect(target);
    room.WaitIdle();
    CHECK(peer.connected());
    CHECK_EQ(peer.node_id(), 2);
    CHECK_EQ(host.total(), 2);

    // ---- boot of the game: ndm:u ------------------------------------------------------------------
    const Handle ndm = ds.Open("ndm:u");
    Reply r = ds.Call(ndm, req::NdmMask(0x14, 0x0F)); // OverrideDefaultDaemons(0xF)
    CHECK_EQ(r.w[0], 0x00140040u);
    CHECK_EQ(r.result(), 0u);
    r = ds.Call(ndm, req::NdmMask(0x06, 0x06)); // SuspendDaemons(6)
    CHECK_EQ(r.w[0], 0x00060040u);
    CHECK_EQ(r.result(), 0u);
    const Handle uds1 = ds.Open("nwm::UDS");
    const Handle uds2 = ds.Open("nwm::UDS"); // the game opens the service twice
    CHECK(uds1 != 0 && uds2 != 0 && uds1 != uds2);

    // ---- multiplayer menu: Enter(LocalCommunications), Initialize, first status read --------------
    r = ds.Call(ndm, req::NdmEnter(2));
    CHECK_EQ(r.w[0], 0x00010040u);
    CHECK_EQ(r.result(), 0u);
    ExpectWords(debug, "ndm cmd 0x01", 3, 1);

    const NodeInfo me = MakeNode(0x371100020a5cc011ULL, "Gionko", 0);
    const std::vector<u32> init = req::Initialize(me, 0x14000, 0x400);
    ExpectGolden(init, {0x00014000, 0x0a5cc011, 0x37110002, 0x00690047, 0x006e006f, 0x006f006b});
    CHECK_EQ(init[12], 0x400u); // InitializeWithVersion(0x400), in the game's code
    r = ds.Call(uds1, init);
    CHECK_EQ(r.w[0], 0x001B0042u);
    CHECK_EQ(r.result(), 0u);
    CHECK_EQ(r.w[2], 0u); // copy-handle descriptor
    const u32 status_event = r.w[3];
    CHECK(status_event != 0);
    ExpectWords(debug, "uds cmd 0x1b", 14, 3);
    CHECK_EQ(kernel.Signals(status_event), 0u); // Initialize does not signal

    r = ds.Call(uds2, req::GetConnectionStatus()); // the game's event thread polls on the 2nd session
    CHECK_EQ(r.w[0], 0x000B0340u);
    CHECK_EQ(r.result(), 0u);
    CHECK_EQ(r.w[2], 3u); // NotConnected
    CHECK_EQ(r.w[3], 0u); // reason
    CHECK_EQ(r.w[4], 0u); // node id | changed << 16
    ExpectWords(debug, "uds cmd 0x0b", 0, 13);

    // ---- lobby search: the beacon of the Azahar host is found, twice ------------------------------
    std::this_thread::sleep_for(std::chrono::milliseconds(250)); // a few beacons arrive first
    std::vector<u8> scan(0x4000);
    const u32 cancel = kernel.CreateEvent(); // the game's cancel handle for the scan, never signalled
    NetworkInfo scanned;
    for (int i = 0; i < 2; ++i) {
        const std::vector<u32> request = req::Scan(scan.data(), 0x4000, kFfeWlanCommId, kFfeNetworkId8, cancel);
        ExpectGolden(request, {0x00004000, 0x00020001, 0x006e0421, 0xffffffff, 0x0000ffff, 0x00000000});
        std::memset(scan.data(), 0, scan.size());
        const auto scan_started = std::chrono::steady_clock::now();
        r = ds.Call(uds1, request);
        const double scan_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - scan_started).count();
        CHECK(scan_ms >= 325 && scan_ms <= 600); // 3 channels x 110 ms, like the chip
        CHECK_EQ(r.w[0], 0x000F0042u);
        CHECK_EQ(r.result(), 0u);
        CHECK_EQ(r.w[2], 0x0004000Cu); // the buffer descriptor comes back
        CHECK_EQ(r.w[3], req::Ptr(scan.data()));
        CHECK_EQ(LoadLE32(scan.data() + 8), 1u); // one network
        CHECK(FirstScannedNetwork(scan, &scanned));
        CHECK_EQ(Hex(scanned.host_mac_address.data(), 6), Hex(host.mac().data(), 6));
        CHECK_EQ(scanned.wlan_comm_id(), kFfeWlanCommId);
        CHECK_EQ(scanned.application_data_size, 148);
        ExpectWords(debug, "uds cmd 0x0f", 20, 3);
    }
    CHECK_EQ(debug.Count("uds cmd 0x0f"), 2);
    CHECK_EQ(kernel.Signals(status_event), 0u); // scanning does not signal either
    // The words of the scan reach the engine: FFE's wlancommid / id8 equal the host's, nothing differs
    // (when it does, the next hardware log says so); the main log has what it asked for
    CHECK_EQ(g_stats.scans, 2u);
    CHECK_EQ(g_stats.scan_entries, 2u);
    CHECK_EQ(g_stats.scan_id_differs, 0u);
    CHECK_EQ(g_stats.scan_odd_input, 0u);
    CHECK_EQ(LogLines("uds: scan 0x0f: type 1 mode 2 channels 0x0421 x 110 ms = 330 ms, bssid ff:ff:ff:ff:ff:ff, "
                      "asks wlancommid 0x0016ed10 id8 0x01 -> 1 network(s), 0 with other ids (left out)"),
             2);
    // A scan whose cancel event is signalled already ends at once (the event is the game's: not closed)
    kernel.SignalEvent(cancel);
    const auto cancelled_started = std::chrono::steady_clock::now();
    r = ds.Call(uds1, req::Scan(scan.data(), 0x4000, kFfeWlanCommId, kFfeNetworkId8, cancel));
    CHECK_EQ(r.result(), 0u);
    const double cancelled_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cancelled_started).count();
    CHECK(cancelled_ms < 100);
    CHECK(!kernel.IsSignaled(cancel)); // (a one-shot event: the scan consumed the signal)

    // ---- join: Bind, Connect, status, application data -------------------------------------------
    std::vector<u32> bind = req::Bind(2, kBindSize, kChannel, 0xffff);
    bind[3] = 0x009120f3; // the capture has stale high bytes in the u8 / u16 words
    bind[4] = 0xffffffff;
    ExpectGolden(bind, {0x00000002, 0x00002e30, 0x009120f3, 0xffffffff});
    r = ds.Call(uds1, bind);
    CHECK_EQ(r.w[0], 0x00120042u);
    CHECK_EQ(r.result(), 0u);
    CHECK_EQ(r.w[2], 0u);
    const u32 bind2_event = r.w[3];
    CHECK(bind2_event != 0);
    ExpectWords(debug, "uds cmd 0x12", 4, 3);

    const std::vector<u32> connect = req::Connect(1, &scanned);
    ExpectGolden(connect, {0x00000001, 0x00000066, 0x00420402});
    r = ds.Call(uds1, connect); // blocks until the Azahar host admitted us
    CHECK_EQ(r.w[0], 0x001E0040u);
    CHECK_EQ(r.result(), 0u);
    room.WaitIdle();
    ExpectWords(debug, "uds cmd 0x1e", 6, 1);
    CHECK_EQ(kernel.Signals(status_event), 1u); // the join signalled the status event once

    r = ds.Call(uds2, req::GetConnectionStatus());
    CHECK_EQ(r.result(), 0u);
    CHECK_EQ(r.w[2], 9u);  // ConnectedAsClient
    CHECK_EQ(r.w[3], 1u);  // NETWORK_IS_AVAILABLE / ConnectionEstablished
    CHECK_EQ(r.w[4], (0x0007u << 16) | 3u); // changed nodes 1,2,3 | node id 3
    ConnectionStatus st = r.Status();
    CHECK_EQ(st.total_nodes, 3);
    CHECK_EQ(st.max_nodes, 4);
    CHECK_EQ(st.node_bitmask, 7);
    CHECK_EQ(st.nodes[0], 1);
    CHECK_EQ(st.nodes[1], 2);
    CHECK_EQ(st.nodes[2], 3);
    r = ds.Call(uds2, req::GetConnectionStatus()); // reading cleared the changed bits
    CHECK_EQ(r.w[4], 3u);

    std::vector<u8> app(0x300);
    for (int i = 0; i < 2; ++i) { // FFE reads it twice, 0.1 s and 0.9 s after joining
        r = ds.Call(uds1, req::GetApplicationData(0xc9), app.data(), static_cast<u32>(app.size()));
        CHECK_EQ(r.w[0], 0x00110082u);
        CHECK_EQ(r.result(), 0u);
        CHECK_EQ(r.w[2], 0x94u);
        CHECK_EQ(r.w[3], IPC_Desc_StaticBuffer(0x94, 0));
        CHECK_EQ(r.w[4], req::Ptr(app.data()));
        CHECK(std::memcmp(app.data(), net.application_data, 148) == 0);
        ExpectWords(debug, "uds cmd 0x11", 1, 4);
    }

    // ---- a further player joins: the game is told by the status event ------------------------------
    target = host.network();
    latecomer.Connect(target);
    room.WaitIdle();
    CHECK(latecomer.connected());
    CHECK_EQ(latecomer.node_id(), 4);
    CHECK_EQ(host.total(), 4);
    CHECK_EQ(kernel.Signals(status_event), 2u);
    r = ds.Call(uds2, req::GetConnectionStatus());
    CHECK_EQ(r.w[2], 9u);
    CHECK_EQ(r.w[4], (0x0008u << 16) | 3u); // node 4 changed
    CHECK_EQ(r.Status().total_nodes, 4);
    CHECK_EQ(r.Status().node_bitmask, 15);

    // ---- play: PullPacket / SendTo streams with FFE's flags --------------------------------------
    // Every sender sends to the 3DS (node 3) with the broadcast flag (one frame per destination, as
    // FFE does); the 3DS sends to the three others the same way. A frame between two clients goes
    // through the Azahar host, which re-broadcasts it: the 3DS sees such a frame once, like a console
    // would (its client drops the host's twin of a frame it delivered); never twice, never none.
    kernel.fast_sleep = true;
    std::map<std::pair<int, int>, int> copies; // (source node, seq) -> times delivered
    u32 frames_pulled = 0;
    auto pull_all = [&]() {
        for (;;) {
            std::vector<u8> out(0x5C8, 0xA5); // (the game's buffer: only a frame may change it)
            Reply p = ds.Call(uds1, req::Pull(2), out.data(), static_cast<u32>(out.size()));
            CHECK_EQ(p.w[0], 0x001400C2u);
            CHECK_EQ(p.result(), 0u);
            // the descriptor has the length the request asked for (0x172 words), whether a frame came or not
            CHECK_EQ(p.w[4], IPC_Desc_StaticBuffer(0x5C8, 0));
            CHECK_EQ(p.w[5], req::Ptr(out.data()));
            if (p.w[2] == 0) {
                CHECK_EQ(p.w[3], 0u);
                CHECK(out == std::vector<u8>(0x5C8, 0xA5)); // an empty pull leaves the buffer alone
                return;
            }
            ++frames_pulled;
            const u32 size = p.w[2];
            const int src = static_cast<int>(p.w[3]);
            CHECK(size >= 2 && size <= 0x5C6);
            CHECK(src == 1 || src == 2 || src == 4);
            CHECK_EQ(out[size], 0xA5u); // ... and a frame changes its own bytes only
            if (size >= 2) {
                CHECK_EQ(out[0], src);
                const std::vector<u8> expected = Payload(static_cast<u8>(src), out[1], size);
                CHECK(std::memcmp(out.data(), expected.data(), size) == 0);
                ++copies[{src, out[1]}];
            }
        }
    };
    const int kRounds = 8;
    for (int round = 0; round < kRounds; ++round) {
        const u8 seq = static_cast<u8>(round);
        const std::size_t size = 73 + round * 9; // the FFE sizes, 73..267 B
        const std::vector<u8> from_peer = Payload(2, seq, size);
        const std::vector<u8> from_late = Payload(4, seq, size);
        const std::vector<u8> from_host = Payload(1, seq, size);
        CHECK(peer.SendTo(3, kChannel, 2, from_peer));
        CHECK(latecomer.SendTo(3, kChannel, 2, from_late));
        CHECK(host.SendData(3, kChannel, 2, from_host));

        const std::vector<u8> mine = Payload(3, seq, size);
        for (u16 dest : {u16(1), u16(2), u16(4)}) {
            const std::vector<u32> send = req::SendTo(dest, kChannel, 2, mine.data(), static_cast<u32>(mine.size()));
            r = ds.Call(uds1, send);
            CHECK_EQ(r.w[0], 0x00170040u);
            CHECK_EQ(r.result(), 0u);
        }
        if (foreign_lobby) {
            strangers_talk(seq);
        }
        room.WaitIdle();
        pull_all();
    }
    ExpectWords(debug, "uds cmd 0x17", 8, 1);
    ExpectWords(debug, "uds PullPacket bind 2", 3, 5);

    for (int src : {1, 2, 4}) {
        for (int round = 0; round < kRounds; ++round) {
            const int n = copies[{src, round}];
            CHECK_EQ(n, 1); // the host's frames, and the clients' (the host's twin is dropped)
        }
    }
    CHECK_EQ(frames_pulled, static_cast<u32>(3 * kRounds));
    // (the counters are the process's, and the other two players are Azahar's, which drop nothing: only the 3DS dropped the
    // host's copy of what the other two clients sent it, 2 flows of kRounds frames)
    CHECK_EQ(g_stats.drop_fwd_dup, static_cast<u32>(2 * kRounds));
    CHECK_EQ(g_stats.fwd_unmatched, 0u);
    // the foreign-lobby filter: a real session has not one frame from a transmitter that is not the host or a node of its map
    // (the other two engines are clients of the same host, so their counters are in here too: all zero); with the stranger
    // in the room every one of its frames was refused, by the 3DS only (the frames are unicast to it)
    CHECK_EQ(g_stats.foreign_shadow, 0u);
    CHECK_EQ(g_stats.drop_foreign, foreign_lobby ? strangers_sent : 0u);
    CHECK_EQ(strangers_sent, foreign_lobby ? static_cast<u32>(3 * kRounds) : 0u);
    CHECK_EQ(debug.Count("FRAME dup"), 2 * kRounds); // each dropped copy is in the capture, without its bytes
    // one event per frame that reached the bind, nothing else signalled it so far
    CHECK_EQ(kernel.Signals(bind2_event), frames_pulled);
    CHECK_EQ(kernel.Signals(status_event), 2u); // the data stream signals no status event

    // what the others got: the host the frames for node 1, the other clients theirs
    const auto inbox = host.TakeInbox();
    CHECK_EQ(inbox.size(), static_cast<std::size_t>(kRounds));
    for (const auto& f : inbox) {
        CHECK_EQ(f.src, 3);
        CHECK_EQ(f.dest, 1);
        CHECK_EQ(f.channel, kChannel);
        CHECK(f.payload.size() >= 2 && f.payload[0] == 3);
    }
    for (AzaharClient* c : {&peer, &latecomer}) {
        // The 3DS's frame twice: once as it was sent (the broadcast flag puts it on the air for everybody) and once as the
        // Azahar host re-broadcasts it. An Azahar client drops neither: the duplicates of the capture's 36 % are real Azahar
        // behaviour, which only the 3DS (a client of ours) filters.
        const std::vector<AzaharClient::Frame> inbox = c->TakeInbox();
        CHECK_EQ(inbox.size(), static_cast<std::size_t>(2 * kRounds));
        for (const AzaharClient::Frame& f : inbox) {
            CHECK_EQ(f.src, 3);
        }
    }
    CHECK_EQ(debug.Count("uds cmd 0x14"), 0); // no PullPacket failed so far: the capture has none of those lines
    // (the Azahar players saw frames addressed to another node only with the broadcast MAC - the host's re-broadcast - never a
    // unicast one: the 3DS, a client, sends none of its own to a MAC that is not the destination's)
    CHECK_EQ(peer.misrouted(), 0u);
    CHECK_EQ(latecomer.misrouted(), 0u);

    // A pull whose buffer is too small is answered "too large" and the frame stays queued
    {
        const std::vector<u8> big = Payload(2, 99, 267);
        CHECK(peer.SendTo(3, kChannel, 2, big));
        room.WaitIdle();
        std::vector<u8> out(0x5C8, 0xA5);
        Reply p = ds.Call(uds1, req::Pull(2, 0x172, 100), out.data(), static_cast<u32>(out.size()));
        CHECK_EQ(p.result(), fw::kBig);
        CHECK(out == std::vector<u8>(0x5C8, 0xA5));
        p = ds.Call(uds1, req::Pull(2, 0x172, 266), out.data(), static_cast<u32>(out.size()));
        CHECK_EQ(p.result(), fw::kBig); // one byte short still
        p = ds.Call(uds1, req::Pull(2), out.data(), static_cast<u32>(out.size()));
        CHECK_EQ(p.result(), 0u);
        CHECK_EQ(p.w[2], 267u);
        CHECK_EQ(p.w[3], 2u);
        CHECK(std::memcmp(out.data(), big.data(), big.size()) == 0);
        ++frames_pulled;
        CHECK_EQ(g_stats.pull_too_large, 2u); // (the two failed pulls are in the capture, all their words)
    }

    // ---- the exit sequence: Unbind(2), Bind(4), status, Disconnect, Shutdown, close, ndm Leave ----
    r = ds.Call(uds1, req::Unbind(2));
    CHECK_EQ(r.w[0], 0x00130140u);
    CHECK_EQ(r.result(), 0u);
    CHECK_EQ(r.w[2], 2u); // the bind id comes back, with its channel and source filter, the frames it stored and pushed out
    CHECK_EQ(r.w[3], (static_cast<u32>(kChannel) << 16) | 0xFFFFu);
    CHECK_EQ(r.w[4], frames_pulled);
    CHECK_EQ(r.w[5], 0u);
    ExpectWords(debug, "uds cmd 0x13", 1, 5);
    CHECK_EQ(kernel.Signals(bind2_event), frames_pulled + 1); // Unbind signals the event once, then closes it

    // The game's other thread pulls from the bind that is gone: the firmware says "no such bind"
    r = ds.Call(uds2, req::Pull(2), nullptr, 0);
    CHECK_EQ(r.result(), fw::kBadId);
    ExpectWords(debug, "uds cmd 0x14", 3, 1); // the failed Pull is in the capture, all its words

    r = ds.Call(uds1, req::Bind(4, 11824, kChannel, 0xffff));
    CHECK_EQ(r.result(), 0u);
    const u32 bind4_event = r.w[3];
    CHECK(bind4_event != 0);
    CHECK_EQ(kernel.Signals(bind4_event), 0u);

    r = ds.Call(uds2, req::GetConnectionStatus()); // the event thread: still a client
    CHECK_EQ(r.w[2], 9u);

    r = ds.Call(uds1, req::Disconnect());
    CHECK_EQ(r.w[0], 0x000A0040u);
    CHECK_EQ(r.result(), 0u);
    CHECK_EQ(kernel.Signals(status_event), 3u);  // the third and last status event
    CHECK_EQ(kernel.Signals(bind4_event), 1u);   // the one bind that is left is woken once: no extras before
    r = ds.Call(uds2, req::GetConnectionStatus());
    CHECK_EQ(r.w[2], 3u);
    CHECK_EQ(r.w[3], 2u); // the reason: REQUEST_FROM_MYSELF
    CHECK_EQ(r.w[4] & 0xFFFF, 3u); // the node id is kept
    CHECK_EQ(r.w[4] >> 16, 0u);
    room.WaitIdle();
    CHECK_EQ(host.departures(), 1u); // the Azahar host dropped us on our Deauthentication

    r = ds.Call(uds1, req::Shutdown());
    CHECK_EQ(r.w[0], 0x00030040u);
    CHECK_EQ(r.result(), 0u);
    CHECK_EQ(kernel.Signals(bind4_event), 2u); // the Shutdown frees the bind that Disconnect left: woken once more
    ds.Close(uds1);
    ds.Close(uds2);
    r = ds.Call(ndm, req::NdmLeave());
    CHECK_EQ(r.w[0], 0x00020040u);
    CHECK_EQ(r.result(), 0u);
    ds.Close(ndm);

    // ---- the debug capture of the whole run -------------------------------------------------------
    CHECK_EQ(debug.Count("ndm cmd 0x14"), 1);
    CHECK_EQ(debug.Count("ndm cmd 0x06"), 1);
    CHECK_EQ(debug.Count("ndm cmd 0x02"), 1);
    CHECK_EQ(debug.Count("uds cmd 0x1e"), 1);
    CHECK_EQ(debug.Count("uds cmd 0x03"), 1);
    CHECK_EQ(debug.Count("uds cmd 0x0a"), 1);
    CHECK(debug.Count("uds cmd 0x17") >= 3 * kRounds); // SendTo is in the capture, flags word included
    CHECK(debug.Last("uds cmd 0x17").find(" req 00000000 ") != std::string::npos);
    CHECK_EQ(debug.Count("uds game opened nwm::UDS"), 2); // the sessions are in the capture, opening and closing
    CHECK_EQ(debug.Count("uds game opened ndm:u"), 1);
    CHECK_EQ(debug.Count("uds game closed its fake nwm::UDS"), 2);
    CHECK(debug.Count("uds role idle -> connecting") >= 1);
    CHECK(debug.Count("uds role connecting -> client") >= 1);

    // The main log has a line for EVERY scan, bind, unbind, connect and status read (not only the
    // first of each), whatever the debug level: as many lines as the capture has calls.
    // (not in the second run of the process: the "first call" tables of NoteCommand and the rate guard are the process')
    if (!foreign_lobby) {
        for (const char* id : {"0x0b", "0x0f", "0x12", "0x13", "0x1e"}) {
            CHECK_EQ(CommandLines(id), debug.Count(std::string("uds cmd ") + id));
        }
        CHECK_EQ(CommandLines("0x0f"), 3); // (the two scans and the cancelled one)
        CHECK(CommandLines("0x0b") >= 5);
        CHECK_EQ(CommandLines("0x11"), 1); // the other commands keep their first-call line only
        CHECK_EQ(CommandLines("0x17"), 1);
        // ... and every command says once, in the first run of the process, which thread first called it.
        CHECK_EQ(LogLines("uds: command 0x0b is first called by thread "), 1);
        CHECK_EQ(LogLines("uds: command 0x0f is first called by thread "), 1);
    }

    // FFE's ndm:u session is closed at the end of the run: what FFE did there is released (Override(0xF) is
    // undone, Suspend(6) is let go) and Boss and Nim are suspended by the default mask again, exactly once.
    const Handle ndm_after = ds.Open("ndm:u");
    r = ds.Call(ndm_after, req::Make(0x16, 0, 0)); // GetDefaultDaemons
    CHECK_EQ(r.result(), 0u);
    CHECK_EQ(r.w[2], 0x09u);
    for (u32 daemon = 0; daemon < 4; ++daemon) {
        r = ds.Call(ndm_after, req::Words(0x0E, 1, daemon)); // GetDaemonDisableCount
        CHECK_EQ(r.result(), 0u);
        CHECK_EQ(r.w[2], 0u);                                // nothing of this session's own
        CHECK_EQ(r.w[3], daemon == 1 || daemon == 2 ? 1u : 0u);
    }
    r = ds.Call(ndm_after, req::Make(0x03, 0, 0)); // QueryExclusiveMode
    CHECK_EQ(r.w[2], 0u);
    ds.Close(ndm_after);

    CHECK(g_rules.inspected() > 80);
    CHECK_RULES();
    if (const char* dump = std::getenv("FFE_REPLAY_DUMP")) { // a sample debug log for the analysis scripts
        CHECK(debug.WriteFile(dump));
    }
}

extern std::atomic<unsigned long long> g_test_sleep_calls; // shim_svc.cpp: svcSleepThread is counted
extern std::atomic<unsigned long long> g_test_sleep_ns;
extern std::atomic<bool> g_test_real_sleep;

// L7: a game thread that calls a command in a tight loop while the service answers it with an error (FFE: PullPacket at 8,000
// calls a second after a sleep stopped the service, 4,000 to 31,000 per 5 s window in PROVA 1) used to be answered
// inline without ever yielding the core. Past 50 failing replies in 100 ms each one now waits 0.5 ms; the reply itself is
// the same word, and successful calls (an empty pull is one) never wait.
static void TestFailingPollIsThrottled() {
    std::printf("failing poll: past 50 failing replies in 100 ms each waits 0.5 ms; the replies and the successful calls are untouched\n");
    MockRoom room;
    HleConsole ds(room, 3);
    const Handle uds = ds.Open("nwm::UDS");
    g_test_real_sleep = false; // (counted, not slept: 5,000 of them would be 2.5 s)
    g_test_sleep_calls = 0;
    g_test_sleep_ns = 0;
    g_stats.hle_throttled = 0;
    const int logged_before = LogLines("uds: a game thread keeps calling commands that fail");

    // the service was never initialised: every PullPacket is "wrong state"
    const auto began = std::chrono::steady_clock::now();
    std::vector<u8> out(0x5C8, 0xA5);
    for (int i = 0; i < 5000; ++i) {
        const Reply r = ds.Call(uds, req::Pull(2), out.data(), static_cast<u32>(out.size()));
        CHECK_EQ(r.w[0], 0x00140040u); // the reply header (a failing reply is the result only) and the result are what they were
        CHECK_EQ(r.result(), fw::kW);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    const unsigned long long slept = g_test_sleep_calls.load();
    // 50 free calls in every window of 100 ms (a slow machine crosses a few)
    const unsigned long long windows = static_cast<unsigned long long>(seconds / 0.1) + 2;
    std::printf("  5,000 failing pulls in %.0f ms: %llu of them waited\n", seconds * 1000, slept);
    CHECK(slept <= 4950);
    CHECK(slept + 50 * windows >= 5000);
    CHECK_EQ(g_test_sleep_ns.load(), slept * 500000ull);
    CHECK_EQ(g_stats.hle_throttled, static_cast<u32>(slept));
    CHECK_EQ(LogLines("uds: a game thread keeps calling commands that fail") - logged_before, 1); // said once

    // successful calls never wait: initialise, then 1,000 status reads (result 0: the poll of FFE's event thread)
    {
        const Reply r = ds.Call(uds, req::Initialize(MakeNode(0x1234, "Throttle", 0), 0x14000, 0x400));
        CHECK_EQ(r.result(), 0u);
    }
    const unsigned long long before = g_test_sleep_calls.load();
    for (int i = 0; i < 1000; ++i) {
        const Reply r = ds.Call(uds, req::GetConnectionStatus());
        CHECK_EQ(r.result(), 0u);
    }
    CHECK_EQ(g_test_sleep_calls.load(), before);

    // a new window: 40 failing replies (a different command: SendTo to a node that is not there) are within the free ones
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const unsigned long long mark = g_test_sleep_calls.load();
    for (int i = 0; i < 40; ++i) {
        const std::vector<u8> data(8, 1);
        const Reply r = ds.Call(uds, req::SendTo(2, kChannel, 2, data.data(), static_cast<u32>(data.size())));
        CHECK(r.result() != 0u);
    }
    CHECK_EQ(g_test_sleep_calls.load(), mark);
    g_test_real_sleep = true;
    ds.Close(uds);
}

// A game that polls GetConnectionStatus (or any of the other every-call commands) must not flood the
// main log: a burst, then two lines a second, each one counting what it left out.
static void TestEveryCallGuard() {
    std::printf("main log: a polling game gets a burst and a trickle\n");
    MockRoom room;
    HleConsole ds(room, 3);
    const Handle uds = ds.Open("nwm::UDS");
    const std::string prefix = "uds: command 0x0b ";
    // the replay before left the guard's bucket part empty: it earns one line per half second
    std::this_thread::sleep_for(std::chrono::milliseconds(4200));
    const int before = LogLines(prefix);
    for (int i = 0; i < 300; ++i) {
        const Reply r = ds.Call(uds, req::GetConnectionStatus());
        CHECK_EQ(r.result(), 0u);
    }
    const int burst = LogLines(prefix) - before;
    CHECK(burst >= 8 && burst <= 10); // 8 tokens, and a refill if the loop crossed a half second
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK_EQ(ds.Call(uds, req::GetConnectionStatus()).result(), 0u);
    CHECK_EQ(LogLines(prefix) - before, burst + 1);
    std::string last;
    {
        std::lock_guard<std::mutex> l(g_log_mutex);
        for (auto it = g_log.rbegin(); it != g_log.rend(); ++it) {
            if (it->compare(0, prefix.size(), prefix) == 0) {
                last = *it;
                break;
            }
        }
    }
    unsigned long call = 0, left_out = 0;
    CHECK_EQ(std::sscanf(last.c_str(), "uds: command 0x0b -> 0x%*x (call %lu, %lu left out before)", &call, &left_out), 2);
    CHECK(left_out >= 280 && left_out <= 292); // 300 polls less the 8..10 that were logged
    ds.Close(uds);
}

int main() {
    LogOn log_on; // (the polling guard test reads the lines of the log)
    SetLogSink([](const char* line) {
        std::lock_guard<std::mutex> l(g_log_mutex);
        g_log.push_back(line);
    });
    Watchdog dog(120);
    dog.Step("the replay");
    TestFfeReplay(false);
    dog.Step("the replay, the foreign-lobby filter on and a stranger in the room");
    TestFfeReplay(true);
    dog.Step("the failing poll");
    TestFailingPollIsThrottled();
    dog.Step("the polling guard");
    TestEveryCallGuard();
    dog.Step("");
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
