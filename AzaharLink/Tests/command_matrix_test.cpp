// Command x state matrix (spec T4 / section 2): the EXACT result of every command, in the argument
// variants of hle_states.hpp, in each state the service can be in. result_closure_test only asks for
// "0 or something the firmware may answer"; this test pins what it answers:
//
//   S0  never initialised / after Shutdown (the real nwm dereferences a null pointer; the HLE says
//       "wrong state")             S3 idle   S6 host   S7 connecting   S9 client   S10 spectator
//   SV1 service stopped by the wireless switch ("wireless off")        SV2 stopped by sleep
//
// Tokens: 0 success, W wrong state 0xC8A113EA, WO wireless off 0xC9411002, BADID 0xE10113EA, BIG
// 0xE10113E9, ARG 0xE10113FD, OOM 0xC86113F3, NF 0xC90113FA, BB 0xE1211005, CF 0xC92113FB, CT
// 0xC92113FE, UNK 0xD900182F. Besides the matrix: the replies' shapes of the commands that were
// stubs, the sessions of the fake service (a closing session that called Initialize shuts the
// service down), the lock-order rule, and a Connect that is cut short from another thread.
//
//   command_matrix_test            (needs no room server; built by Tests/ipc_test/build.sh)
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "azl/log.hpp"

#include "hle_states.hpp"

using namespace azl;

// ---------------------------------------------------------------------------------------------
// The matrix. Columns: S0 S3 S6 S7 S9 S10 SV1 SV2
// ---------------------------------------------------------------------------------------------
struct Row {
    u32 id;
    const char* variant;
    const char* expect;
};

static const Row kRows[] = {
    {0x01, "ok", "0 0 0 0 0 0 WO W"}, // a first call initialises, every later one is a no-op success
    {0x1B, "ok", "0 0 0 0 0 0 WO W"},
    {0x1B, "unaligned-memory", "OOM 0 0 0 0 0 WO W"}, // (the size is only looked at by the first call)
    {0x02, "ok", "0 0 0 0 0 0 0 0"},
    {0x03, "ok", "0 0 0 0 0 0 0 0"},
    {0x04, "ok", "W 0 W W W W WO W"},
    {0x1D, "ok", "W 0 W W W W WO W"},
    {0x1D, "one-node", "W BADID W W W W WO W"},
    {0x1D, "appdata-201", "W BIG W W W W WO W"},
    {0x05, "node-3", "W W 0 W W W WO W"}, // an unknown node is no error
    {0x05, "node-1", "W BADID BADID BADID BADID BADID WO W"}, // (the host can't be kicked: before the state)
    {0x05, "all", "W W 0 W W W WO W"},
    {0x06, "ok", "W W 0 W W W WO W"},
    {0x07, "block-new", "W 0 0 0 0 0 WO W"},
    {0x08, "ok", "W W 0 W W W WO W"},
    {0x09, "ok", "W CT W W W W WO W"}, // (nobody answers: the wait ends at once in this harness)
    {0x1E, "client", "W CT W W W W WO W"},
    {0x1E, "spectator", "W CT W W W W WO W"},
    {0x1E, "bad-type", "W ARG W W W W WO W"},
    {0x0A, "ok", "W 0 W 0 0 0 WO W"}, // a host that calls it gets "wrong state"
    {0x0B, "ok", "0 0 0 0 0 0 0 0"},
    {0x0C, "ok", "W W 0 0 0 0 WO W"},
    {0x0D, "node-1", "W NF 0 NF 0 0 WO W"},
    {0x0D, "node-9", "W NF NF NF NF NF WO W"},
    {0x0D, "node-0", "W 0 0 0 0 0 WO W"}, // (the empty slot: found in every state, zeros)
    {0x0E, "garbage", "BB BB BB BB BB BB BB BB"}, // pure function: never gated
    {0x1F, "garbage", "BB BB BB BB BB BB BB BB"},
    {0x0F, "scan", "W 0 W W W W WO W"},
    {0x10, "10-bytes", "W W 0 W W W WO W"},
    {0x10, "201-bytes", "W W BIG W W W WO W"},
    {0x10, "null-0", "W W 0 W W W WO W"},
    {0x11, "ok", "W W 0 W 0 0 WO W"},
    {0x12, "ok", "W 0 0 0 0 0 WO W"},
    {0x12, "id-0", "W BADID BADID BADID BADID BADID WO W"},
    {0x12, "channel-0", "W BADID BADID BADID BADID BADID WO W"},
    {0x12, "small-buffer", "W OOM OOM OOM OOM OOM WO W"},
    {0x13, "bound", "0 0 0 0 0 0 0 0"},
    {0x13, "unknown", "0 0 0 0 0 0 0 0"},
    {0x13, "id-0", "0 BADID BADID BADID BADID BADID 0 0"}, // (no service task: nothing to unbind)
    {0x14, "bound-empty", "W W 0 W 0 0 WO W"},
    {0x14, "unknown", "W W BADID W BADID BADID WO W"},
    {0x14, "id-0", "W W BADID W BADID BADID WO W"},
    {0x15, "10ms", "W 0 W W W W WO W"},
    {0x15, "1ms", "W ARG ARG ARG ARG ARG WO W"}, // (the argument is checked before the state)
    {0x16, "profile-0", "W W 0 W 0 0 WO W"},
    {0x16, "profile-3", "W ARG ARG ARG ARG ARG WO W"},
    {0x17, "to-host", "W W NF W 0 W WO W"},
    {0x17, "to-self", "W W NF W NF W WO W"},
    {0x17, "to-zero", "W W 0 W 0 W WO W"}, // nodes that are not there: no error, no frame
    {0x17, "to-absent", "W W 0 W 0 W WO W"},
    {0x17, "oversize", "W BIG BIG BIG BIG BIG WO W"}, // (the size is checked before the state)
    {0x18, "1500", "W 0 W W W W W W"}, // (the stopped service answers "wrong state", never "wireless off")
    {0x18, "0x20000", "W ARG W W W W W W"},
    {0x19, "10", "W 0 W W W W W W"},
    {0x19, "0x20000", "W ARG W W W W W W"},
    {0x1A, "ok", "W 0 0 0 0 0 WO W"},
    {0x1C, "ok", "W 0 0 0 0 0 WO W"},
    {0x20, "aggregator-0", "W W 0 W 0 W WO W"},
    {0x21, "ok", "W 0 W W W W W W"},
    {0x22, "ok", "W W W W 0 0 WO W"},
    {0x23, "ok", "0 0 0 0 0 0 0 0"},
    {0x00, "id-0", "UNK UNK UNK UNK UNK UNK UNK UNK"},
    {0x24, "id-0x24", "UNK UNK UNK UNK UNK UNK UNK UNK"},
};

static std::string Name(u32 code) {
    static const std::map<u32, const char*> names = {
        {0, "0"},          {fw::kW, "W"},           {fw::kWO, "WO"},           {fw::kBadId, "BADID"},
        {fw::kBig, "BIG"}, {fw::kArg, "ARG"},       {fw::kOom, "OOM"},         {fw::kNodeNotFound, "NF"},
        {fw::kBadBeacon, "BB"}, {fw::kConnFailed, "CF"}, {fw::kConnTimeout, "CT"}, {fw::kBadCmd, "UNK"},
        {fw::kNoHost, "NOHOST"}, {fw::kNetFull, "FULL"}, {fw::kAgg, "AGG"},
    };
    const auto it = names.find(code);
    if (it != names.end()) {
        return it->second;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(code));
    return buf;
}

static std::vector<std::string> Split(const char* s) {
    std::vector<std::string> out;
    std::string cur;
    for (; *s; ++s) {
        if (*s == ' ') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += *s;
        }
    }
    out.push_back(cur);
    return out;
}

static void TestMatrix() {
    std::printf("command x state matrix: the exact result of every cell\n");
    const std::vector<Variant> variants = Variants();
    Watchdog dog(30);
    int cells = 0;
    for (const Variant& variant : variants) {
        const Row* row = nullptr;
        for (const Row& candidate : kRows) {
            if (candidate.id == variant.id && std::strcmp(candidate.variant, variant.name) == 0) {
                row = &candidate;
            }
        }
        ++g_checks;
        if (!row) {
            ++g_failures;
            std::fprintf(stderr, "  FAIL no expected row for command 0x%02lx '%s'\n",
                         static_cast<unsigned long>(variant.id), variant.name);
            continue;
        }
        const std::vector<std::string> expect = Split(row->expect);
        ++g_checks;
        if (expect.size() != AllStates().size()) {
            ++g_failures;
            std::fprintf(stderr, "  FAIL row 0x%02lx '%s' has %zu columns\n", static_cast<unsigned long>(variant.id),
                         variant.name, expect.size());
            continue;
        }
        for (std::size_t s = 0; s < AllStates().size(); ++s) {
            const State state = AllStates()[s];
            dog.Step(CellName("cell", state, variant));
            g_rules.Reset();
            Env env;
            const u32 result = RunCell(env, state, variant).result();
            ++cells;
            ++g_checks;
            if (Name(result) != expect[s]) {
                ++g_failures;
                std::fprintf(stderr, "  FAIL %s: got %s, the firmware says %s\n", CellName("cell", state, variant).c_str(),
                             Name(result).c_str(), expect[s].c_str());
            }
            CHECK(g_rules.violations().empty()); // and nothing the command sent upsets an Azahar peer
        }
    }
    dog.Step("");
    for (const Row& row : kRows) {
        bool used = false;
        for (const Variant& variant : variants) {
            used |= row.id == variant.id && std::strcmp(row.variant, variant.name) == 0;
        }
        CHECK(used); // (a row for a variant that no longer exists)
    }
    std::printf("  %d cells\n", cells);
}

// ---------------------------------------------------------------------------------------------
// Replies of the commands that used to be stubs (the shape a game reads)
// ---------------------------------------------------------------------------------------------
static void TestReplyShapes() {
    std::printf("replies of 0x0C, 0x1A, 0x23, unknown ids, 0x22\n");
    MockRoom room;
    HleConsole ds(room, 3);
    ds.kernel.fast_sleep = true;
    const Handle uds = ds.Open("nwm::UDS");
    Reply r = ds.Call(uds, req::Initialize(MakeNode(0xABCD, "Shapes", 0), 0x14000, 0x400));
    CHECK_EQ(r.result(), 0u);

    // 0x23: the allowed-channel mask, in every state, header 0x00230080
    r = ds.Call(uds, req::Make(0x23, 0, 0));
    CHECK_EQ(r.w[0], 0x00230080u);
    CHECK_EQ(r.result(), 0u);
    CHECK_EQ(r.w[2], 0x07FFu);

    // 0x1A: 0 before any network, then the channel of the last one (also after it ended)
    r = ds.Call(uds, req::Make(0x1A, 0, 0));
    CHECK_EQ(r.w[0], 0x001A0080u);
    CHECK_EQ(r.result(), 0u);
    CHECK_EQ(r.w[2], 0u);
    NetworkInfo own = MakeNetworkInfo(4, 6);
    own.channel = 6;
    CHECK_EQ(ds.Call(uds, req::BeginHosting(&own)).result(), 0u);
    CHECK_EQ(ds.Call(uds, req::Make(0x1A, 0, 0)).w[2], 6u);

    // 0x0C: header 0x000C0280; nobody else in the network yet: all zero
    r = ds.Call(uds, req::Make(0x0C, 0, 0));
    CHECK_EQ(r.w[0], 0x000C0280u);
    CHECK_EQ(r.result(), 0u);
    for (int i = 2; i <= 10; ++i) {
        CHECK_EQ(r.w[i], 0u);
    }
    // a client joins (an Azahar one): its slot reads {sample 40, valid}, the host's own slot nothing
    Console peer(room, 4);
    u32 ev;
    CHECK_EQ(peer.engine->Initialize(MakeNode(0x4444, "Peer", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    NetworkInfo target;
    std::vector<u8> scan(0x4000);
    ds.console.engine->Tick(platform::NowMs() + 100000);
    room.WaitIdle();
    CHECK_EQ(peer.engine->RecvBeaconBroadcastData(scan.data(), static_cast<u32>(scan.size()), kBroadcastMac), 0u);
    CHECK(FirstScannedNetwork(scan, &target));
    CHECK_EQ(peer.engine->ConnectToNetwork(reinterpret_cast<u8*>(&target), sizeof(target), 1), 0u);
    room.WaitIdle();
    r = ds.Call(uds, req::Make(0x0C, 0, 0));
    CHECK_EQ(r.result(), 0u);
    CHECK_EQ(r.w[2], 0x0128u);       // the current peer
    CHECK_EQ(r.w[3], 0x01280000u);   // slots 0 (the host itself: nothing) and 1 (the peer)
    for (int i = 4; i <= 10; ++i) {
        CHECK_EQ(r.w[i], 0u);
    }
    CHECK_EQ(ds.Call(uds, req::Shutdown()).result(), 0u);

    // 0x22 while connected: the reply hands the buffer descriptor back, like 0x0F
    {
        AzaharHost host(room, 1, MakeNetworkInfo(4, 4), MakeNode(0x1111, "AzHost", 0));
        CHECK_EQ(ds.Call(uds, req::Initialize(MakeNode(0xABCD, "Shapes", 0), 0x14000, 0x400)).result(), 0u);
        host.SendBeacon();
        room.WaitIdle();
        NetworkInfo net = host.network();
        CHECK_EQ(ds.Call(uds, req::Connect(1, &net)).result(), 0u);
        room.WaitIdle();
        std::vector<u8> buffer(0x4000);
        host.SendBeacon();
        room.WaitIdle();
        r = ds.Call(uds, req::ScanOnConnection(buffer.data(), static_cast<u32>(buffer.size()), 0, 0xFF));
        CHECK_EQ(r.w[0], 0x00220042u);
        CHECK_EQ(r.result(), 0u);
        CHECK_EQ(r.w[2], (static_cast<u32>(buffer.size()) << 4) | 0xC);
        CHECK_EQ(r.w[3], req::Ptr(buffer.data()));
        CHECK_EQ(ds.Call(uds, req::Disconnect()).result(), 0u);
        CHECK_EQ(ds.Call(uds, req::Shutdown()).result(), 0u);
    }

    // ids the service does not have: reply header 0x40 and 0xD900182F
    for (u32 id : {0u, 0x24u, 0x25u, 0xFFFFu}) {
        r = ds.Call(uds, std::vector<u32>{id << 16});
        CHECK_EQ(r.w[0], 0x00000040u);
        CHECK_EQ(r.result(), 0xD900182Fu);
    }
    ds.Close(uds);
}

// 0x15 / 0x18 / 0x19 argument boundaries (the firmware's ranges)
static void TestStubArguments() {
    std::printf("argument ranges of 0x15, 0x16, 0x18, 0x19\n");
    MockRoom room;
    HleConsole ds(room, 3);
    const Handle uds = ds.Open("nwm::UDS");
    CHECK_EQ(ds.Call(uds, req::Initialize(MakeNode(0xABCD, "Range", 0), 0x14000, 0x400)).result(), 0u);
    // SetMaxSendDelay: 5 ms to 100 ms, as a 64-bit number of nanoseconds in words 1 and 2
    CHECK_EQ(ds.Call(uds, req::Words(0x15, 2, 5000000, 0)).result(), 0u);
    CHECK_EQ(ds.Call(uds, req::Words(0x15, 2, 4999999, 0)).result(), fw::kArg);
    CHECK_EQ(ds.Call(uds, req::Words(0x15, 2, 100000000, 0)).result(), 0u);
    CHECK_EQ(ds.Call(uds, req::Words(0x15, 2, 100000001, 0)).result(), fw::kArg);
    CHECK_EQ(ds.Call(uds, req::Words(0x15, 2, 10000000, 1)).result(), fw::kArg); // high word set: 4.3 s
    // the beacon-miss time: 1024 to 51200 time units
    CHECK_EQ(ds.Call(uds, req::Words(0x18, 1, 1024)).result(), 0u);
    CHECK_EQ(ds.Call(uds, req::Words(0x18, 1, 1023)).result(), fw::kArg);
    CHECK_EQ(ds.Call(uds, req::Words(0x18, 1, 51200)).result(), 0u);
    CHECK_EQ(ds.Call(uds, req::Words(0x18, 1, 51201)).result(), fw::kArg);
    CHECK_EQ(ds.Call(uds, req::Words(0x18, 1, 0)).result(), fw::kArg);
    // the access point parameter: below 0x20000
    CHECK_EQ(ds.Call(uds, req::Words(0x19, 1, 0x1FFFF)).result(), 0u);
    CHECK_EQ(ds.Call(uds, req::Words(0x19, 1, 0x20000)).result(), fw::kArg);
    // the radio profile: 0, 1, 2 (only in a session)
    CHECK_EQ(ds.Call(uds, req::Words(0x16, 1, 2)).result(), fw::kW);
    CHECK_EQ(ds.Call(uds, req::Words(0x16, 1, 3)).result(), fw::kArg);
    // libctru's udsCreateNetwork sends command 0x21 with its own header word and data 0: accepted
    CHECK_EQ(ds.Call(uds, req::Words(0x21, 2, 0x00210080, 0)).result(), 0u);
    CHECK_EQ(ds.Call(uds, req::Words(0x21, 2, 0, 1)).result(), 0u); // and no word is validated
    ds.Close(uds);
}

// ---------------------------------------------------------------------------------------------
// The fake sessions: a closing session that called Initialize shuts the service down
// ---------------------------------------------------------------------------------------------
static void TestSessionClose() {
    std::printf("session close = Shutdown (a session that called Initialize)\n");
    MockRoom room;
    HleConsole ds(room, 3);
    SentFrames sent(ds.console);
    CountingKernel& kernel = ds.kernel;
    const Handle uds1 = ds.Open("nwm::UDS");
    const Handle uds2 = ds.Open("nwm::UDS");
    const Handle ndm = ds.Open("ndm:u");
    Reply r = ds.Call(uds1, req::Initialize(MakeNode(0xABCD, "Close", 0), 0x14000, 0x400));
    CHECK_EQ(r.result(), 0u);
    const u32 status_event = r.w[3];
    NetworkInfo own = MakeNetworkInfo(4, 6);
    CHECK_EQ(ds.Call(uds1, req::BeginHosting(&own)).result(), 0u);
    const u32 bind_event = ds.Call(uds1, req::Bind(2, 0x2E30, 0xF3, 0xFFFF)).w[3];
    CHECK(ds.console.engine->status() == NetworkStatus::ConnectedAsHost);
    CHECK_EQ(kernel.Signals(status_event), 1u); // BeginHosting

    // the sessions that never called Initialize, and the ndm:u one, close without any effect
    ds.Close(uds2);
    ds.Close(ndm);
    CHECK(ds.console.engine->initialized());
    CHECK(ds.console.engine->status() == NetworkStatus::ConnectedAsHost);
    CHECK_EQ(sent.Count(WifiPacket::Type::Deauthentication), 0);

    // closing the session that did: the host ends like at Shutdown (3 Deauthentications, one status
    // event, the bind freed), and its beacons stop
    ds.console.engine->Tick(platform::NowMs() + 100000); // (one goes out first)
    CHECK_EQ(sent.Count(WifiPacket::Type::Beacon), 1);
    ds.Close(uds1);
    CHECK(!ds.console.engine->initialized());
    CHECK(ds.console.engine->status() == static_cast<NetworkStatus>(0));
    CHECK_EQ(sent.CountTo(WifiPacket::Type::Deauthentication, kBroadcastMac), 3);
    CHECK_EQ(kernel.Signals(status_event), 2u);
    CHECK_EQ(kernel.Signals(bind_event), 2u); // woken when the session ends and again when the bind is freed
    ds.console.engine->Tick(platform::NowMs() + 200000);
    CHECK_EQ(sent.Count(WifiPacket::Type::Beacon), 1);
    room.WaitIdle();
    CHECK_RULES();

    // A session whose Initialize was refused counts as well: the service was stopped (sleep), the
    // second session's Initialize fails, and its close is the Shutdown that makes the service usable
    const Handle a = ds.Open("nwm::UDS");
    const Handle b = ds.Open("nwm::UDS");
    CHECK_EQ(ds.Call(a, req::Initialize(MakeNode(0xABCD, "Close", 0), 0x14000, 0x400)).result(), 0u);
    ds.console.engine->StopService(kServiceSleep);
    CHECK_EQ(ds.Call(b, req::Initialize(MakeNode(0xABCD, "Close", 0), 0x14000, 0x400)).result(), fw::kW);
    ds.Close(b);
    CHECK(!ds.console.engine->initialized());
    CHECK_EQ(ds.Call(a, req::Initialize(MakeNode(0xABCD, "Close", 0), 0x14000, 0x400)).result(), 0u);
    ds.Close(a);
    CHECK(!ds.console.engine->initialized());
}

// ---------------------------------------------------------------------------------------------
// Lock order: UdsHle::mutex_ is never held while the engine runs. The exit of FFE has a Pull and an
// Unbind on two threads within a millisecond, and a session close racing a Pull must not deadlock.
// ---------------------------------------------------------------------------------------------
static void TestCloseRacesPull() {
    std::printf("session close racing PullPacket / SendTo on other threads\n");
    MockRoom room;
    HleConsole ds(room, 3);
    ds.kernel.fast_sleep = true;
    const Handle uds1 = ds.Open("nwm::UDS");
    const Handle uds2 = ds.Open("nwm::UDS");
    CHECK_EQ(ds.Call(uds1, req::Initialize(MakeNode(0xABCD, "Race", 0), 0x14000, 0x400)).result(), 0u);
    NetworkInfo own = MakeNetworkInfo(4, 6);
    CHECK_EQ(ds.Call(uds1, req::BeginHosting(&own)).result(), 0u);
    CHECK_EQ(ds.Call(uds1, req::Bind(2, 0x2E30, 0xF3, 0xFFFF)).result(), 0u);
    std::atomic<bool> stop{false};
    std::atomic<int> calls{0};
    std::thread puller([&] {
        std::vector<u8> out(0x5C8);
        while (!stop.load()) {
            const Reply p = ds.Call(uds2, req::Pull(2), out.data(), static_cast<u32>(out.size()));
            const u32 rc = p.result();
            // while it lasts: success; after the bind or the session is gone: BADID, then wrong state
            CHECK(rc == 0 || rc == fw::kBadId || rc == fw::kW);
            ++calls;
        }
    });
    for (int i = 0; i < 2000 && calls.load() < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(calls.load() >= 50);
    ds.Close(uds1); // Shutdown, from this thread, while the other one is inside the engine
    const int after = calls.load();
    for (int i = 0; i < 2000 && calls.load() < after + 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(calls.load() >= after + 20); // still running: nothing deadlocked
    stop = true;
    puller.join();
    CHECK(!ds.console.engine->initialized());
    ds.Close(uds2);
}

// The sleep_stop=1 path: Plugin::OnSleepEnter calls the engine's StopService on the game's APT thread while other
// game threads are inside PullPacket / SendTo and the network thread delivers its callbacks (a lost link, the
// room's member list). Nothing may deadlock, and every answer is one the firmware gives.
static void TestStopServiceRaces() {
    std::printf("service stop (sleep) racing PullPacket, SendTo and the network thread's callbacks\n");
    g_rules.Reset();
    MockRoom room;
    HleConsole ds(room, 5);
    ds.kernel.fast_sleep = true;
    const Handle uds = ds.Open("nwm::UDS");
    const Handle uds2 = ds.Open("nwm::UDS");
    const u8 payload[32] = {1, 2, 3};
    std::atomic<u32> wrong{0};
    auto allowed = [&wrong](u32 rc) {
        // 0, or what the firmware answers in the other states: wrong state, wireless off, no such bind
        if (rc != 0 && rc != fw::kW && rc != fw::kWO && rc != fw::kBadId) {
            wrong = rc;
        }
    };
    for (int round = 0; round < 40; ++round) {
        CHECK_EQ(ds.Call(uds, req::Shutdown()).result(), 0u); // the way out of a stopped service
        CHECK_EQ(ds.Call(uds, req::Initialize(MakeNode(0xABCD, "Sleep", 0), 0x14000, 0x400)).result(), 0u);
        NetworkInfo own = MakeNetworkInfo(4, 6);
        CHECK_EQ(ds.Call(uds, req::BeginHosting(&own)).result(), 0u);
        CHECK_EQ(ds.Call(uds, req::Bind(2, 0x2E30, 0xF3, 0xFFFF)).result(), 0u);
        std::atomic<bool> stop{false};
        std::atomic<int> calls{0};
        std::thread puller([&] {
            std::vector<u8> out(0x5C8);
            while (!stop.load()) {
                allowed(ds.Call(uds2, req::Pull(2), out.data(), static_cast<u32>(out.size())).result());
                ++calls;
            }
        });
        std::thread sender([&] {
            while (!stop.load()) {
                allowed(ds.Call(uds2, req::SendTo(0xFFFF, 0xF3, 2, payload, sizeof(payload))).result());
                ++calls;
            }
        });
        std::thread network([&] { // the room client's thread
            while (!stop.load()) {
                ds.console.engine->OnTransportDown(NetworkStatusChangeReason::ConnectionLost);
                ds.console.engine->OnRoomMembers({ds.console.mac});
                ++calls;
            }
        });
        for (int i = 0; i < 2000 && calls.load() < 30; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ds.console.engine->StopService(round % 2 ? kServiceSleep : kServiceWirelessOff); // the game's APT thread
        const int after = calls.load();
        for (int i = 0; i < 2000 && calls.load() < after + 30; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(calls.load() >= after + 30); // nothing deadlocked
        stop = true;
        puller.join();
        sender.join();
        network.join();
        // stopped: everything is refused until Shutdown
        CHECK_EQ(ds.Call(uds, req::BeginHosting(&own)).result(), round % 2 ? fw::kW : fw::kWO);
    }
    CHECK_EQ(wrong.load(), 0u);
    CHECK_EQ(ds.Call(uds, req::Shutdown()).result(), 0u);
    CHECK_RULES();
    ds.Close(uds);
    ds.Close(uds2);
}

// ---------------------------------------------------------------------------------------------
// Connect cut short from other threads (the held Connect of state S7)
// ---------------------------------------------------------------------------------------------
static void TestConnectAborted() {
    std::printf("a Connect in progress ends at once with 0xC92113FB when the game disconnects or shuts down\n");
    for (const bool disconnect : {true, false}) {
        g_rules.Reset();
        Env env;
        env.Enter(S7);
        CHECK_EQ(env.connect_result.load(), 0u);
        const u64 started = platform::NowMs();
        const Reply r = env.ds->Call(env.uds, disconnect ? req::Disconnect() : req::Shutdown());
        CHECK_EQ(r.result(), 0u);
        env.Release(); // joins the Connect thread
        CHECK(platform::NowMs() - started < 2000); // (not the 5 s of the timeout)
        CHECK_EQ(env.connect_result.load(), fw::kConnFailed);
        CHECK(env.ds->console.engine->status() == (disconnect ? NetworkStatus::NotConnected : static_cast<NetworkStatus>(0)));
        if (disconnect) {
            const ConnectionStatus st = env.ds->Call(env.uds, req::GetConnectionStatus()).Status();
            CHECK_EQ(st.status, 3u);
            CHECK_EQ(st.status_change_reason, 2u); // REQUEST_FROM_MYSELF: the game's own Disconnect won
        }
        CHECK_RULES(); // and no Deauthentication went to a host that never answered
    }
}

// A game the HLE cannot give a fake session to (the process is out of kernel events, or all 32 slots are in use)
// is left to the real service: that is logged as a warning and counted, and the plugin shows the count on screen.
extern std::atomic<bool> g_fail_create_event; // Tests/ipc_test/shim_svc.cpp
static void TestSessionRefused() {
    std::printf("a session the HLE cannot open is counted and logged (the real service answers)\n");
    LogOn log_on;
    std::vector<std::string> lines;
    SetLogSink([&lines](const char* line) { lines.push_back(line); });
    MockRoom room;
    HleConsole ds(room, 4);
    g_test_hle = ds.hle.get();
    std::memset(&g_stats, 0, sizeof(g_stats));
    Handle h = 0;
    g_fail_create_event = true;
    CHECK(R_FAILED(srvGetServiceHandle(&h, "nwm::UDS"))); // not served: the request goes on to the real service
    g_fail_create_event = false;
    CHECK_EQ(g_stats.hle_declined, 1u);
    std::vector<Handle> open;
    for (int i = 0; i < 32; ++i) { // every slot of the fake sessions
        open.push_back(ds.Open("nwm::UDS"));
    }
    CHECK_EQ(g_stats.hle_declined, 1u);
    CHECK(R_FAILED(srvGetServiceHandle(&h, "ndm:u")));
    CHECK_EQ(g_stats.hle_declined, 2u);
    for (const Handle o : open) {
        ds.Close(o);
    }
    Handle again = ds.Open("ndm:u"); // the slots are free again
    ds.Close(again);
    SetLogSink([](const char*) {});
    int warnings = 0;
    for (const std::string& line : lines) {
        warnings += line.find("WARNING no ") != std::string::npos;
    }
    CHECK_EQ(warnings, 2);
}

extern std::atomic<bool> g_test_real_sleep; // Tests/ipc_test/shim_svc.cpp
int main() {
    SetLogSink([](const char*) {});
    // The throttle of a failing-reply spin (L7) counts its pauses here and does not sleep them: the race loops below count their
    // iterations while the other threads run, and 500 us per failing reply would turn most of them into waiting (it is
    // ffe_replay_test that checks the throttle itself).
    g_test_real_sleep = false;
    Watchdog dog(180);
    dog.Step("the matrix");
    TestMatrix();
    dog.Step("reply shapes");
    TestReplyShapes();
    dog.Step("argument ranges");
    TestStubArguments();
    dog.Step("session close");
    TestSessionClose();
    dog.Step("close racing pull");
    TestCloseRacesPull();
    dog.Step("service stop racing");
    TestStopServiceRaces();
    dog.Step("connect aborted");
    TestConnectAborted();
    dog.Step("sessions refused");
    TestSessionRefused();
    dog.Step("");
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
