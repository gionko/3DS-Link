// Host-side tests of the suppression of the copies an Azahar host re-broadcasts (uds_dedupe.hpp and the block in
// UdsEngine::HandleSecureDataPacket).
//   1. ForwardDedupe on its own, with explicit clocks
//   2. engines through the mock room with a model of an Azahar host (it re-broadcasts every frame it receives
//      for another node): who receives how many copies for every way of sending
//   3. the rules of the filter, frame by frame: keys, window, roles, a new session, a host that is not Azahar
//   4. optional: replay of a real capture converted to a CSV (trace1.csv trace2.csv ...: "t_ms dir src dst ch
//      size role hex"): dedupe_test <csv>...
// Build & run: Tests/run.sh
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "azl/stats.hpp"
#include "azl/uds_dedupe.hpp"
#include "azahar_peer.hpp"

// ---------------------------------------------------------------------------------------------
// 0. CopyDedupe: the copy a radio_like=2 sender adds, by the sequence number of the frame
// ---------------------------------------------------------------------------------------------
static void TestCopyDedupeUnit() {
    std::printf("CopyDedupe\n");
    CopyDedupe d;
    CHECK(!d.Seen(2, 1));  // the first time: remembered
    CHECK(d.Seen(2, 1));   // the copy: dropped
    CHECK(d.Seen(2, 1));   // (a third copy too)
    CHECK(!d.Seen(2, 2));  // the next frame
    CHECK(!d.Seen(3, 1));  // another node's number 1
    CHECK(!d.Seen(2, 0) && !d.Seen(2, 0)); // 0 = unnumbered (Azahar, an older plugin): never a copy
    CHECK(!d.Seen(0, 5) && !d.Seen(0, 5)); // node 0 does not exist: never a copy
    CHECK(!d.Seen(17, 5) && !d.Seen(17, 5)); // out of the table: never a copy
    // the window: the last 128 numbers of a node; the 129th pushes the first out
    for (u16 s = 100; s < 228; ++s) {
        CHECK(!d.Seen(4, s));
    }
    CHECK(d.Seen(4, 100) && d.Seen(4, 227));
    CHECK(!d.Seen(4, 228)); // (the 129th pushes 100 out)
    CHECK(!d.Seen(4, 100)); // (pushed out: seen again as new, and that pushes 101 out)
    CHECK(d.Seen(4, 102) && d.Seen(4, 228) && d.Seen(4, 100));
    CHECK(!d.Seen(4, 101));
    // a sender's batch: 64 originals, then their 64 copies after as many broadcast frames of the same node: every copy is a copy
    CopyDedupe batch;
    for (u16 s = 1; s <= 64; ++s) {
        CHECK(!batch.Seen(6, s));
    }
    for (u16 s = 65; s <= 128; ++s) {
        CHECK(!batch.Seen(6, s)); // (broadcast frames between the originals and their copies)
    }
    int dropped = 0;
    for (u16 s = 1; s <= 64; ++s) {
        dropped += batch.Seen(6, s) ? 1 : 0;
    }
    CHECK_EQ(dropped, 64);
    // the wrap of the number: 65535 then 1 (SendTo skips 0)
    CHECK(!d.Seen(5, 65535) && !d.Seen(5, 1) && d.Seen(5, 65535) && d.Seen(5, 1));
    // a node that left is forgotten, the others are not
    d.ForgetNode(2);
    CHECK(!d.Seen(2, 1) && !d.Seen(2, 2));
    CHECK(d.Seen(3, 1));
    d.Reset();
    CHECK(!d.Seen(3, 1) && !d.Seen(4, 227));
}

// ---------------------------------------------------------------------------------------------
// 1. ForwardDedupe
// ---------------------------------------------------------------------------------------------
static void TestDedupeUnit() {
    std::printf("ForwardDedupe\n");
    {
        ForwardDedupe d;
        CHECK(!d.ConsumeForwarded(3, 100, 0xAAAA, 1000)); // a forwarded copy alone: delivered
        CHECK(!d.NoteDirect(3, 100, 0xAAAA, 1000));
        CHECK(d.ConsumeForwarded(3, 100, 0xAAAA, 1002));  // its twin: dropped
        CHECK(!d.ConsumeForwarded(3, 100, 0xAAAA, 1003)); // a third copy: delivered
    }
    {
        ForwardDedupe d; // d1 d2 f1 f2: the same payload sent twice, both direct copies survive
        d.NoteDirect(3, 73, 0x1234, 500);
        d.NoteDirect(3, 73, 0x1234, 500);
        CHECK(d.ConsumeForwarded(3, 73, 0x1234, 501));
        CHECK(d.ConsumeForwarded(3, 73, 0x1234, 502));
        CHECK(!d.ConsumeForwarded(3, 73, 0x1234, 503));
    }
    {
        ForwardDedupe d; // the key: sender, size, hash
        d.NoteDirect(3, 100, 7, 10);
        CHECK(!d.ConsumeForwarded(2, 100, 7, 11));
        CHECK(!d.ConsumeForwarded(3, 101, 7, 11));
        CHECK(!d.ConsumeForwarded(3, 100, 8, 11));
        CHECK(d.ConsumeForwarded(3, 100, 7, 11));
    }
    {
        ForwardDedupe d; // the window: 250 ms
        d.NoteDirect(3, 100, 7, 1000);
        CHECK(!d.ConsumeForwarded(3, 100, 7, 1251));
        d.NoteDirect(3, 100, 7, 2000);
        CHECK(d.ConsumeForwarded(3, 100, 7, 2250));
    }
    {
        ForwardDedupe d; // the 32-bit clock wraps
        d.NoteDirect(3, 100, 7, 0xFFFFFFF0u);
        CHECK(d.ConsumeForwarded(3, 100, 7, 0x00000010u));
        d.NoteDirect(3, 100, 9, 0xFFFFFF00u);
        CHECK(!d.ConsumeForwarded(3, 100, 9, 0x00000200u)); // 0x300 ms later: expired
    }
    {
        ForwardDedupe d; // a full table forgets the oldest entry: its twin is delivered, the others still match
        for (u32 i = 0; i < ForwardDedupe::kSlots; ++i) {
            CHECK(!d.NoteDirect(3, 100, 1000 + i, 100 + i));
        }
        CHECK(d.NoteDirect(3, 100, 5000, 200)); // true: a live entry made room
        CHECK(!d.ConsumeForwarded(3, 100, 1000, 201)); // the evicted (oldest) one
        CHECK(d.ConsumeForwarded(3, 100, 1001, 201));
        CHECK(d.ConsumeForwarded(3, 100, 5000, 201));
    }
    {
        ForwardDedupe d; // an expired entry is reused without counting as an eviction
        for (u32 i = 0; i < ForwardDedupe::kSlots; ++i) {
            d.NoteDirect(3, 100, 1000 + i, 100);
        }
        CHECK(!d.NoteDirect(3, 100, 9999, 100 + ForwardDedupe::kWindowMs + 1));
    }
    {
        ForwardDedupe d;
        d.NoteDirect(3, 100, 7, 10);
        d.Reset();
        CHECK(!d.ConsumeForwarded(3, 100, 7, 11));
    }
    {
        // the hash depends on every byte, on the length and on the trailing bytes
        u8 a[23], b[23];
        for (int i = 0; i < 23; ++i) {
            a[i] = static_cast<u8>(i * 7 + 1);
        }
        std::memcpy(b, a, sizeof(a));
        CHECK_EQ(HashFrame(a, 23), HashFrame(b, 23));
        for (int i = 0; i < 23; ++i) {
            b[i] ^= 1;
            CHECK(HashFrame(a, 23) != HashFrame(b, 23));
            b[i] ^= 1;
        }
        CHECK(HashFrame(a, 22) != HashFrame(a, 23));
        CHECK(HashFrame(a, 0) != HashFrame(a, 1));
    }
    CHECK_EQ(sizeof(ForwardDedupe), std::size_t(ForwardDedupe::kSlots * 16)); // 1 KB, no allocation
}

// ---------------------------------------------------------------------------------------------
// 2. Engines through the mock room
// ---------------------------------------------------------------------------------------------
struct Got {
    u16 src;
    std::vector<u8> bytes;
};

static std::vector<Got> Drain(Console& c, u32 bind_id) {
    std::vector<Got> v;
    for (int i = 0; i < 4096; ++i) {
        u16 src = 0;
        u32 size = 0;
        std::vector<u8> out;
        CHECK_EQ(PullInto(*c.engine, bind_id, &out, &size, &src), 0u);
        if (size == 0) {
            break;
        }
        out.resize(size);
        v.push_back({src, std::move(out)});
    }
    return v;
}

static Mac NodeMac(u8 n) {
    return Mac{0x00, 0x1F, 0x32, 0x00, 0x00, n}; // the scheme of Console(room, mac_last)
}

// A frame as the room hands it to an engine.
static WifiPacket MakeData(const Mac& transmitter, const Mac& dest_mac, u16 src_node, u16 dest_node, u8 channel,
                           const std::vector<u8>& payload, u16 seq = 0) {
    WifiPacket p;
    p.type = WifiPacket::Type::Data;
    p.channel = 11;
    p.transmitter_address = transmitter;
    p.destination_address = dest_mac;
    p.data = GenerateDataPayload(payload.data(), payload.size(), channel, dest_node, src_node, seq);
    return p;
}

// An Azahar host (node 1) and `clients` consoles of ours (nodes 2, 3, ...), each with a bind on channels 1 and 243.
struct Net {
    MockRoom room;
    std::unique_ptr<AzaharHost> host;
    std::vector<std::unique_ptr<Console>> cl; // cl[0] = node 2, ...

    explicit Net(int clients) {
        host.reset(new AzaharHost(room, 1, MakeNetworkInfo(8, 0), MakeNode(0x1111, "Host", 0)));
        for (int i = 0; i < clients; ++i) {
            AddClient();
        }
    }
    Console& C(int node) {
        return *cl[node - 2];
    }
    void AddClient() {
        const int node = static_cast<int>(cl.size()) + 2;
        cl.emplace_back(new Console(room, static_cast<u8>(node)));
        Console& c = *cl.back();
        u32 ev;
        CHECK_EQ(c.engine->Initialize(MakeNode(0x2000 + node, "Client", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        Connect(c);
        CHECK_EQ(c.engine->Bind(2, 0x5F4, 1, kBroadcastNetworkNodeId, &ev), 0u);
        CHECK_EQ(c.engine->Bind(3, 0x5F4, 243, kBroadcastNetworkNodeId, &ev), 0u);
    }
    void Connect(Console& c) {
        NetworkInfo ni = host->network();
        CHECK_EQ(c.engine->ConnectToNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni), 1), 0u);
        room.WaitIdle();
    }
};

static Stats Snap() {
    return g_stats;
}

/// The debug capture so far, as the lines of AzaharLink.debug.log (the time reads 0).
static std::vector<std::string> DebugLinesNow() {
    std::vector<std::string> lines;
    DebugRecord rec;
    u8 buf[1700];
    while (DebugPop(&rec, buf, sizeof(buf))) {
        std::string line;
        DebugFormat(rec, buf, rec.ms, &line);
        lines.push_back(line);
    }
    return lines;
}

static void TestEngineDedupe() {
    std::printf("engines: who receives how many copies (Azahar host, clients 2, 3, 4)\n");
    g_rules.Reset();
    std::memset(&g_stats, 0, sizeof(g_stats));
    Net net(3);
    Console &A = net.C(2), &B = net.C(3), &Cc = net.C(4);
    CHECK_EQ(A.engine->GetConnectionStatus().network_node_id, 2);
    CHECK_EQ(B.engine->GetConnectionStatus().network_node_id, 3);
    CHECK_EQ(Cc.engine->GetConnectionStatus().network_node_id, 4);
    for (Console* c : {&A, &B, &Cc}) {
        Drain(*c, 2);
    }
    const std::vector<u8> m1 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

    // A -> B with the broadcast flag (FFE): the room gives B the direct copy, the host re-broadcasts, B sees ONE
    {
        const Stats s0 = Snap();
        CHECK(DebugInit());
        g_debug_level = 1;
        DebugLinesNow();
        CHECK_EQ(A.engine->SendTo(3, 1, m1.size(), 2, m1.data(), m1.size()), 0u);
        net.room.WaitIdle();
        g_debug_level = 0;
        // the capture has the delivered copy with its bytes and the dropped one as a header-only record
        int rx = 0, dup = 0;
        for (const std::string& line : DebugLinesNow()) {
            rx += line.find("FRAME rx    2>3 ch1 10B: 0102030405060708090a\n") != std::string::npos;
            dup += line.find("FRAME dup   2>3 ch1 10B: \n") != std::string::npos;
        }
        CHECK_EQ(rx, 1);
        CHECK_EQ(dup, 1);
        const auto b = Drain(B, 2);
        CHECK_EQ(b.size(), std::size_t(1));
        if (!b.empty()) {
            CHECK_EQ(b[0].src, 2);
            CHECK(b[0].bytes == m1);
        }
        CHECK_EQ(Drain(Cc, 2).size() + Drain(A, 2).size(), std::size_t(0));
        CHECK_EQ(net.host->forwarded(), 1u);
        const Stats s1 = Snap();
        CHECK_EQ(s1.fwd_candidates - s0.fwd_candidates, 1u);
        CHECK_EQ(s1.drop_fwd_dup - s0.drop_fwd_dup, 1u);
        CHECK_EQ(s1.fwd_unmatched - s0.fwd_unmatched, 0u);
        CHECK_EQ(s1.data_delivered - s0.data_delivered, 1u);
    }
    // A -> B without the flag: unicast to B's MAC, the host never sees it, nothing to filter
    {
        const Stats s0 = Snap();
        CHECK_EQ(A.engine->SendTo(3, 1, m1.size(), 0, m1.data(), m1.size()), 0u);
        net.room.WaitIdle();
        CHECK_EQ(Drain(B, 2).size(), std::size_t(1));
        CHECK_EQ(Drain(Cc, 2).size() + Drain(A, 2).size(), std::size_t(0));
        CHECK_EQ(net.host->forwarded(), 1u); // (still the one of before)
        CHECK_EQ(Snap().fwd_candidates - s0.fwd_candidates, 0u);
    }
    // the host sends to B (broadcast flag): its own frame, one copy, no candidate
    {
        const Stats s0 = Snap();
        CHECK(net.host->SendData(3, 1, 2, m1));
        net.room.WaitIdle();
        const auto b = Drain(B, 2);
        CHECK_EQ(b.size(), std::size_t(1));
        if (!b.empty()) {
            CHECK_EQ(b[0].src, 1);
        }
        CHECK_EQ(Drain(A, 2).size() + Drain(Cc, 2).size(), std::size_t(0));
        CHECK_EQ(Snap().fwd_candidates - s0.fwd_candidates, 0u);
    }
    // A -> the host with the broadcast flag: the host takes it, nobody else is addressed
    {
        CHECK_EQ(A.engine->SendTo(1, 1, m1.size(), 2, m1.data(), m1.size()), 0u);
        net.room.WaitIdle();
        CHECK_EQ(net.host->TakeInbox().size(), std::size_t(1));
        CHECK_EQ(Drain(B, 2).size() + Drain(Cc, 2).size() + Drain(A, 2).size(), std::size_t(0));
    }
    // A -> everybody (node 0xFFFF): one copy each, no forwarding involved
    {
        const Stats s0 = Snap();
        CHECK_EQ(A.engine->SendTo(kBroadcastNetworkNodeId, 1, m1.size(), 0, m1.data(), m1.size()), 0u);
        net.room.WaitIdle();
        CHECK_EQ(Drain(B, 2).size(), std::size_t(1));
        CHECK_EQ(Drain(Cc, 2).size(), std::size_t(1));
        CHECK_EQ(net.host->TakeInbox().size(), std::size_t(1));
        CHECK_EQ(Snap().fwd_candidates - s0.fwd_candidates, 0u);
    }
    // the same bytes sent twice in a row (a legitimate repeat): B gets BOTH
    {
        CHECK_EQ(A.engine->SendTo(3, 1, m1.size(), 2, m1.data(), m1.size()), 0u);
        CHECK_EQ(A.engine->SendTo(3, 1, m1.size(), 2, m1.data(), m1.size()), 0u);
        net.room.WaitIdle();
        CHECK_EQ(Drain(B, 2).size(), std::size_t(2));
    }
    // a burst of different frames, A -> B and C -> B interleaved: every one arrives once, in order per sender
    {
        for (u8 i = 0; i < 20; ++i) {
            const std::vector<u8> m = {i, 0xA5, 0x5A}, m2 = {i, 0xC3, 0x3C};
            CHECK_EQ(A.engine->SendTo(3, 1, m.size(), 2, m.data(), m.size()), 0u);
            CHECK_EQ(Cc.engine->SendTo(3, 1, m2.size(), 2, m2.data(), m2.size()), 0u);
        }
        net.room.WaitIdle();
        const auto b = Drain(B, 2);
        CHECK_EQ(b.size(), std::size_t(40));
        int na = 0, nc = 0;
        for (const Got& g : b) {
            if (g.src == 2) {
                CHECK_EQ(g.bytes[0], na);
                ++na;
            } else if (g.src == 4) {
                CHECK_EQ(g.bytes[0], nc);
                ++nc;
            }
        }
        CHECK_EQ(na, 20);
        CHECK_EQ(nc, 20);
    }
    // the debug capture of a dropped copy has a direction of its own and carries no bytes
    {
        DebugRecord rec{};
        std::string line;
        rec.kind = kDbgFrame;
        rec.dir = kDirDup;
        rec.src = 2;
        rec.dest = 3;
        rec.channel = 1;
        rec.size = 10;
        DebugFormat(rec, nullptr, 0, &line);
        CHECK_EQ(line, std::string("     0.000 FRAME dup   2>3 ch1 10B: \n"));
    }
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------
// 3. The rules, frame by frame
// ---------------------------------------------------------------------------------------------
static void TestRules() {
    std::printf("engine: a forwarded copy without a direct copy is delivered; keys; window; roles; sessions\n");
    g_rules.Reset();
    std::memset(&g_stats, 0, sizeof(g_stats));
    Net net(3);
    Console &A = net.C(2), &B = net.C(3), &Cc = net.C(4);
    const Mac host_mac = net.host->mac();
    for (Console* c : {&A, &B, &Cc}) {
        Drain(*c, 2);
        Drain(*c, 3);
    }
    // the only copy there is: the host forwards what a client sent to the host's own MAC
    {
        const Stats s0 = Snap();
        g_debug_level = 1;
        DebugLinesNow();
        WifiPacket f = MakeData(host_mac, kBroadcastMac, 2, 3, 1, {9, 9, 9});
        B.engine->OnWifiPacket(f);
        g_debug_level = 0;
        CHECK_EQ(Drain(B, 2).size(), std::size_t(1));
        CHECK_EQ(Snap().fwd_unmatched - s0.fwd_unmatched, 1u);
        CHECK_EQ(Snap().fwd_candidates - s0.fwd_candidates, 1u);
        // the capture says it was delivered alone
        int alone = 0, rx = 0;
        for (const std::string& line : DebugLinesNow()) {
            alone += line.find("uds twin: node 2>3 ch1 3B came through the host with no direct copy: delivered") != std::string::npos;
            rx += line.find("FRAME rx    2>3 ch1 3B: 090909") != std::string::npos;
        }
        CHECK_EQ(alone, 1);
        CHECK_EQ(rx, 1);
    }
    // direct, then forwarded: the second is dropped
    {
        WifiPacket d = MakeData(A.mac, kBroadcastMac, 2, 3, 1, {8, 8, 8});
        WifiPacket f = MakeData(host_mac, kBroadcastMac, 2, 3, 1, {8, 8, 8});
        B.engine->OnWifiPacket(d);
        B.engine->OnWifiPacket(f);
        CHECK_EQ(Drain(B, 2).size(), std::size_t(1));
    }
    // forwarded first, direct later: both are delivered (the direct one is a new frame as far as anyone can tell)
    {
        WifiPacket d = MakeData(A.mac, kBroadcastMac, 2, 3, 1, {8, 7, 8});
        WifiPacket f = MakeData(host_mac, kBroadcastMac, 2, 3, 1, {8, 7, 8});
        B.engine->OnWifiPacket(f);
        B.engine->OnWifiPacket(d);
        CHECK_EQ(Drain(B, 2).size(), std::size_t(2));
    }
    // the whole frame is the key, the sequence number included: the same payload under another sequence number
    // is another frame
    {
        WifiPacket d7 = MakeData(A.mac, kBroadcastMac, 2, 3, 1, {7, 7}, 7);
        WifiPacket f8 = MakeData(host_mac, kBroadcastMac, 2, 3, 1, {7, 7}, 8);
        WifiPacket f7 = MakeData(host_mac, kBroadcastMac, 2, 3, 1, {7, 7}, 7);
        B.engine->OnWifiPacket(d7);
        B.engine->OnWifiPacket(f8);
        CHECK_EQ(Drain(B, 2).size(), std::size_t(2)); // d7 and f8
        B.engine->OnWifiPacket(f7);
        CHECK_EQ(Drain(B, 2).size(), std::size_t(0)); // f7 pairs with d7
    }
    // another channel is not a twin, another sender is not a twin
    {
        WifiPacket dch = MakeData(A.mac, kBroadcastMac, 2, 3, 243, {6, 6});
        WifiPacket fch = MakeData(host_mac, kBroadcastMac, 2, 3, 1, {6, 6});
        B.engine->OnWifiPacket(dch);
        B.engine->OnWifiPacket(fch);
        CHECK_EQ(Drain(B, 3).size(), std::size_t(1));
        CHECK_EQ(Drain(B, 2).size(), std::size_t(1));
        WifiPacket dsrc = MakeData(Cc.mac, kBroadcastMac, 4, 3, 1, {5, 5});
        WifiPacket fsrc = MakeData(host_mac, kBroadcastMac, 2, 3, 1, {5, 5});
        B.engine->OnWifiPacket(dsrc);
        B.engine->OnWifiPacket(fsrc);
        CHECK_EQ(Drain(B, 2).size(), std::size_t(2));
    }
    // frames of the host (node 1) are never candidates, identical twice or not
    {
        const Stats s0 = Snap();
        WifiPacket h1 = MakeData(host_mac, kBroadcastMac, 1, 3, 1, {4, 4});
        B.engine->OnWifiPacket(h1);
        B.engine->OnWifiPacket(h1);
        CHECK_EQ(Drain(B, 2).size(), std::size_t(2));
        CHECK_EQ(Snap().fwd_candidates - s0.fwd_candidates, 0u);
    }
    // frames to B's own MAC are not candidates, nor are frames for node 0xFFFF
    {
        const Stats s0 = Snap();
        WifiPacket u1 = MakeData(A.mac, B.mac, 2, 3, 1, {3, 3});
        WifiPacket u2 = MakeData(host_mac, B.mac, 2, 3, 1, {3, 3});
        B.engine->OnWifiPacket(u1);
        B.engine->OnWifiPacket(u2);
        WifiPacket a1 = MakeData(A.mac, kBroadcastMac, 2, kBroadcastNetworkNodeId, 1, {2, 2});
        WifiPacket a2 = MakeData(host_mac, kBroadcastMac, 2, kBroadcastNetworkNodeId, 1, {2, 2});
        B.engine->OnWifiPacket(a1);
        B.engine->OnWifiPacket(a2);
        CHECK_EQ(Drain(B, 2).size(), std::size_t(4));
        CHECK_EQ(Snap().fwd_candidates - s0.fwd_candidates, 0u);
    }
    // a frame that arrives later than the window is delivered
    {
        WifiPacket d = MakeData(A.mac, kBroadcastMac, 2, 3, 1, {2, 2, 2});
        WifiPacket f = MakeData(host_mac, kBroadcastMac, 2, 3, 1, {2, 2, 2});
        B.engine->OnWifiPacket(d);
        std::this_thread::sleep_for(std::chrono::milliseconds(ForwardDedupe::kWindowMs + 60));
        B.engine->OnWifiPacket(f);
        CHECK_EQ(Drain(B, 2).size(), std::size_t(2));
    }
    // a new session forgets the pending twins: a direct copy, leave, join again, then its forwarded twin shows up
    {
        WifiPacket d = MakeData(A.mac, kBroadcastMac, 2, 3, 1, {1, 1, 1});
        WifiPacket f = MakeData(host_mac, kBroadcastMac, 2, 3, 1, {1, 1, 1});
        B.engine->OnWifiPacket(d);
        Drain(B, 2);
        CHECK_EQ(B.engine->DisconnectNetwork(), 0u);
        net.room.WaitIdle();
        net.Connect(B);
        CHECK_EQ(B.engine->GetConnectionStatus().network_node_id, 3);
        B.engine->OnWifiPacket(f);
        CHECK_EQ(Drain(B, 2).size(), std::size_t(1));
        // ... and the same for the end of a session without a new Connect in between
        B.engine->OnWifiPacket(d);
        Drain(B, 2);
        CHECK_EQ(B.engine->DisconnectNetwork(), 0u);
        net.room.WaitIdle();
        net.Connect(B);
        B.engine->OnWifiPacket(f);
        CHECK_EQ(Drain(B, 2).size(), std::size_t(1));
    }
    // the host of another session (another MAC) is not "the host": its copies are direct ones
    {
        WifiPacket d = MakeData(A.mac, kBroadcastMac, 2, 3, 1, {0, 1, 0});
        WifiPacket other = MakeData(NodeMac(77), kBroadcastMac, 2, 3, 1, {0, 1, 0});
        B.engine->OnWifiPacket(d);
        B.engine->OnWifiPacket(other);
        CHECK_EQ(Drain(B, 2).size(), std::size_t(2));
    }
    // a console that hosts is no candidate at all: frames are never filtered there
    {
        MockRoom room;
        Console H(room, 1), X(room, 2);
        u32 ev, hev;
        CHECK_EQ(H.engine->Initialize(MakeNode(1, "H", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        NetworkInfo ni = MakeNetworkInfo(4, 0);
        CHECK_EQ(H.engine->BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni)), 0u);
        CHECK_EQ(H.engine->Bind(2, 0x5F4, 1, kBroadcastNetworkNodeId, &hev), 0u);
        // (X is a client of H: a host takes a frame only from a client it admitted)
        H.engine->Tick(platform::NowMs() + 100000); // a beacon
        room.WaitIdle();
        u32 xev;
        CHECK_EQ(X.engine->Initialize(MakeNode(2, "X", 0), kTestSharedMemory, kTestVersion, &xev), 0u);
        std::vector<u8> scan(0x4000);
        NetworkInfo joined;
        CHECK_EQ(X.engine->RecvBeaconBroadcastData(scan.data(), static_cast<u32>(scan.size()), kBroadcastMac), 0u);
        CHECK(FirstScannedNetwork(scan, &joined));
        CHECK_EQ(X.engine->ConnectToNetwork(reinterpret_cast<u8*>(&joined), sizeof(joined), 1), 0u);
        room.WaitIdle();
        const Stats s0 = Snap();
        WifiPacket d = MakeData(X.mac, kBroadcastMac, 2, 1, 1, {1, 2});
        WifiPacket f = MakeData(H.mac, kBroadcastMac, 2, 1, 1, {1, 2});
        H.engine->OnWifiPacket(d);
        H.engine->OnWifiPacket(f);
        CHECK_EQ(Drain(H, 2).size(), std::size_t(2));
        CHECK_EQ(Snap().fwd_candidates - s0.fwd_candidates, 0u);
    }
    CHECK_RULES();
}

// A console of ours that hosts routes only what was sent to its own MAC (F05); the client that gets such a
// frame has no direct copy to pair it with: it is delivered, once.
static void TestOurHost() {
    std::printf("engines: a 3DS host forwards a frame sent to its MAC, the client delivers it once\n");
    g_rules.Reset();
    std::memset(&g_stats, 0, sizeof(g_stats));
    MockRoom room;
    Console H(room, 1), A(room, 2), B(room, 3);
    u32 ev;
    CHECK_EQ(H.engine->Initialize(MakeNode(1, "H", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    NetworkInfo hosting = MakeNetworkInfo(4, 0);
    CHECK_EQ(H.engine->BeginHostingNetwork(reinterpret_cast<u8*>(&hosting), sizeof(hosting)), 0u);
    for (Console* c : {&A, &B}) {
        CHECK_EQ(c->engine->Initialize(MakeNode(2, "C", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        NetworkInfo ni = MakeNetworkInfo(4, 0);
        ni.host_mac_address = H.mac;
        ni.channel = 11;
        CHECK_EQ(c->engine->ConnectToNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni), 1), 0u);
        room.WaitIdle();
        CHECK_EQ(c->engine->Bind(2, 0x5F4, 1, kBroadcastNetworkNodeId, &ev), 0u);
    }
    // a frame for node 3 addressed to the host's MAC (a sender that lets the host route it)
    const Stats s0 = Snap();
    WifiPacket p = MakeData(A.mac, H.mac, 2, 3, 1, {7, 7, 7});
    H.engine->OnWifiPacket(p);
    room.WaitIdle();
    CHECK_EQ(Drain(B, 2).size(), std::size_t(1));
    CHECK_EQ(Drain(A, 2).size(), std::size_t(0));
    CHECK_EQ(Snap().forwarded - s0.forwarded, 1u);
    CHECK_EQ(Snap().fwd_unmatched - s0.fwd_unmatched, 1u);
    CHECK_EQ(Snap().drop_fwd_dup - s0.drop_fwd_dup, 0u);
    // a broadcast-MAC frame is not routed by it any more: B gets the direct copy only
    p = MakeData(A.mac, kBroadcastMac, 2, 3, 1, {6, 6, 6});
    H.engine->OnWifiPacket(p);
    B.engine->OnWifiPacket(p);
    room.WaitIdle();
    CHECK_EQ(Drain(B, 2).size(), std::size_t(1));
    CHECK_EQ(Snap().forwarded - s0.forwarded, 1u);
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------
// 4. Replay of a real capture (trace CSV): "t_ms dir src dst ch size role hex"
//    role: H host-sourced / broadcast, D first copy, F second copy (host-forwarded), S lone copy, O frame for another node
// ---------------------------------------------------------------------------------------------
static void ReplayTrace(const char* path) {
    std::ifstream in(path);
    if (!in) {
        std::printf("replay: cannot open %s\n", path);
        ++g_failures;
        return;
    }
    struct Rec {
        u32 t;
        std::string dir;
        u16 src, dst;
        u8 ch;
        u32 size;
        char role;
        std::vector<u8> payload;
    };
    std::vector<Rec> recs;
    u16 me = 0;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        Rec r;
        unsigned src, dst, ch, size;
        std::string role, hex;
        ss >> r.t >> r.dir >> src >> dst >> ch >> size >> role >> hex;
        r.src = static_cast<u16>(src);
        r.dst = static_cast<u16>(dst);
        r.ch = static_cast<u8>(ch);
        r.size = size;
        r.role = role[0];
        r.payload = FromHex(hex.c_str());
        if (r.dir == "rx" && r.dst != 0xFFFF && me == 0) {
            me = r.dst;
        }
        recs.push_back(std::move(r));
    }
    std::printf("replay %s: %zu records, receiving node %u\n", path, recs.size(), me);
    CHECK(me >= 2);

    // the replaying engine must be node `me`: connect me-2 filler clients first
    g_rules.Reset();
    Net net(static_cast<int>(me) - 1);
    Console& R = net.C(me);
    CHECK_EQ(R.engine->GetConnectionStatus().network_node_id, me);
    u32 bev;
    CHECK_EQ(R.engine->Bind(7, 0x5F4, 243, kBroadcastNetworkNodeId, &bev), 0u);
    Drain(R, 7);
    const Mac host_mac = net.host->mac();
    auto mac_of = [&](u16 node) { return node == 1 ? host_mac : NodeMac(static_cast<u8>(node)); };

    u64 rx_total = 0, expect_delivered = 0, got_delivered = 0, mismatches = 0, f_total = 0;
    const Stats s0 = Snap();
    for (const Rec& r : recs) {
        if (r.dir != "rx" && r.dir != "other") {
            continue;
        }
        const bool fwd = r.role == 'F';
        // H: sent by the host (transmitter = host MAC) or broadcast; D/S/O: transmitter = the original sender;
        // F: the host's re-broadcast of someone else's frame
        const Mac tx = fwd ? host_mac : mac_of(r.src);
        WifiPacket p = MakeData(tx, kBroadcastMac, r.src, r.dst, r.ch, r.payload);
        R.engine->OnWifiPacket(p);
        const auto got = Drain(R, 7);
        if (r.dir == "rx") {
            ++rx_total;
            f_total += fwd ? 1 : 0;
            const std::size_t want = fwd ? 0 : 1;
            expect_delivered += want;
            got_delivered += got.size();
            if (got.size() != want || (!got.empty() && (got[0].bytes != r.payload || got[0].src != r.src))) {
                ++mismatches;
            }
        } else if (!got.empty()) {
            ++mismatches; // frames for other nodes are never delivered
        }
    }
    const Stats s1 = Snap();
    std::printf("  rx records %llu: second copies %llu; delivered %llu (expected %llu); mismatches %llu\n",
                (unsigned long long)rx_total, (unsigned long long)f_total, (unsigned long long)got_delivered,
                (unsigned long long)expect_delivered, (unsigned long long)mismatches);
    std::printf("  engine counters: candidates %u, dropped as duplicates %u, delivered alone %u\n",
                s1.fwd_candidates - s0.fwd_candidates, s1.drop_fwd_dup - s0.drop_fwd_dup,
                s1.fwd_unmatched - s0.fwd_unmatched);
    CHECK_EQ(mismatches, 0u);
    CHECK_EQ(got_delivered, expect_delivered);
    CHECK_EQ(u64(s1.drop_fwd_dup - s0.drop_fwd_dup), f_total);
}

int main(int argc, char** argv) {
    TestCopyDedupeUnit();
    TestDedupeUnit();
    TestEngineDedupe();
    TestRules();
    TestOurHost();
    for (int i = 1; i < argc; ++i) {
        ReplayTrace(argv[i]);
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
