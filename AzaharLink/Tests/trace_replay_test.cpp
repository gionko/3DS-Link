// Replays of the real hardware traces (the captures of the user's console, 2026-10-10), through UdsHle - the IPC front-end
// a game talks to - with Azahar emulators around the 3DS that are the FROZEN client model of azahar_peer.hpp.
//
//   FFE as host   PROVA 2: Final Fantasy Explorers hosts a lobby of 4 on the 3DS. The request words of every command, as the
//                 capture logged them, are sent again and the reply words must be the ones the 3DS gave then; the payloads
//                 of the capture are the frames that the three PCs send and that the 3DS sends back.
//   TFH as host   the first trace of Zelda Tri Force Heroes (a lobby of 3, UpdateNetworkAttribute, no DestroyNetwork).
//   lid episode   PROVA 2 at the time the lid was shut: the lobby of 4, the three PCs leaving, the SECOND lobby of the game
//                 that they made in the same room (its node ids are ours; 501 of its frames overflowed the ring of the 3DS and
//                 fired the "frames lost" warning of 0.1.15) and the PCs coming back. L4 of 0.1.16: the host takes a frame only
//                 from a client it admitted.
//   client stretch  PROVA 1: the 3DS as a client of a PC host; nothing of it may be counted as foreign, filter on or off.
//
// What is real and what is not: the command words, the reply words, the payload bytes, the sizes, the sources and destinations
// and the order of the frames are the capture's (Tests/make_goldens.py cut them out of the 20-65 MB logs); the payload bytes of
// the lid episode are filler (the filter reads the transmitter and the node ids, never the payload); the MAC of the PC called
// Friend-F was not captured and is made up.
//
//   trace_replay_test          (needs no room server; built by Tests/ipc_test/build.sh)
#include <chrono>
#include <cstdlib>
#include <functional>
#include <set>
#include <thread>

#include "azl/log.hpp"

#include "hle_harness.hpp"

using namespace azl;

// ---- the shapes of the generated data (Tests/make_goldens.py) --------------------------------------------------
struct GoldenCmd {
    const char* tag;
    u8 id;
    u8 n_req; ///< words after the header
    u8 n_rep;
    u32 req[16];
    u32 rep[16];
};
struct GoldenNetwork {
    u32 wlancommid;
    u8 id8;
    u16 attributes;
    u32 network_id;
    u8 max_nodes;
    u8 app_size;
    const char* app_hex;
};
struct GoldenFrame {
    u16 src, dst, size;
    bool rx; ///< a payload the 3DS pulled (words = the PullPacket reply) or one it sent (words = the SendTo request)
    u32 words[8];
    const char* hex;
};
struct TraceFrame {
    char kind; ///< R received for this node, O for another node, T sent by the 3DS
    u16 src, dst, size;
};

#include "ffe_host_golden.inc"
#include "tfh_host_golden.inc"
#include "lobby_trace.inc"

static const u8 kChannel = 0xF3;
static const std::size_t kWindow = 0x5C8; // the buffer a game gives PullPacket

template <std::size_t N>
static const GoldenCmd& Cmd(const GoldenCmd (&table)[N], const char* tag) {
    for (const GoldenCmd& c : table) {
        if (std::strcmp(c.tag, tag) == 0) {
            return c;
        }
    }
    std::fprintf(stderr, "  FAIL: no golden command '%s'\n", tag);
    ++g_failures;
    return table[0];
}

/// Words of a request that the header counts as translate parameters (handles, buffers, process ids).
static unsigned TranslateWords(bool ndm, u32 id) {
    if (ndm) {
        return id == 0x01 || id == 0x02 ? 2 : 0;
    }
    switch (id) {
    case 0x1B: return 2; // Initialize: the shared memory handle
    case 0x1D: return 4; // BeginHostingNetwork: the NetworkInfo and the passphrase
    case 0x17: return 2; // SendTo: the payload
    case 0x10: return 2; // SetApplicationData
    }
    return 0;
}

static std::vector<u32> RequestOf(const GoldenCmd& g, bool ndm) {
    const unsigned t = TranslateWords(ndm, g.id);
    std::vector<u32> v = req::Make(g.id, g.n_req - t, t);
    for (unsigned i = 0; i < g.n_req; ++i) {
        v[1 + i] = g.req[i];
    }
    return v;
}

/// The reply against the capture: the header names the command and as many words as the capture has, the words are the
/// capture's (the first `compare` of them: the rest are handles and pointers of the capture's process).
static void ExpectReply(const GoldenCmd& g, const Reply& r, unsigned compare) {
    CHECK_EQ(r.w[0] >> 16, static_cast<u32>(g.id));
    CHECK_EQ(((r.w[0] >> 6) & 0x3F) + (r.w[0] & 0x3F), static_cast<u32>(g.n_rep));
    for (unsigned i = 0; i < compare && i < g.n_rep; ++i) {
        if (r.w[1 + i] != g.rep[i]) {
            ++g_failures;
            std::fprintf(stderr, "  FAIL %s: reply word %u is 0x%08lx, the capture has 0x%08lx\n", g.tag, i,
                         static_cast<unsigned long>(r.w[1 + i]), static_cast<unsigned long>(g.rep[i]));
        }
        ++g_checks;
    }
}

static Reply Replay(HleConsole& ds, Handle h, const GoldenCmd& g, bool ndm = false, unsigned compare = 99,
                    const std::function<void(std::vector<u32>&)>& patch = nullptr) {
    std::vector<u32> request = RequestOf(g, ndm);
    if (patch) {
        patch(request);
    }
    const Reply r = ds.Call(h, request);
    ExpectReply(g, r, compare);
    return r;
}

static NodeInfo NodeOf(const GoldenCmd& node_info_reply) {
    u8 raw[kNodeInfoSize];
    for (int i = 0; i < 10; ++i) {
        StoreLE32(raw + 4 * i, node_info_reply.rep[1 + i]);
    }
    NodeInfo n;
    n.Load(raw);
    return n;
}

static NetworkInfo HostedNetwork(const GoldenNetwork& g) {
    NetworkInfo ni;
    std::memset(&ni, 0, sizeof(ni));
    ni.max_nodes = g.max_nodes;
    StoreBE32(ni.wlan_comm_id_be, g.wlancommid);
    ni.id = g.id8;
    StoreBE16(ni.attributes_be, g.attributes);
    StoreBE32(ni.network_id_be, g.network_id);
    ni.application_data_size = g.app_size;
    const std::vector<u8> app = FromHex(g.app_hex);
    CHECK_EQ(app.size(), static_cast<std::size_t>(g.app_size));
    std::memcpy(ni.application_data, app.data(), std::min<std::size_t>(app.size(), sizeof(ni.application_data)));
    return ni;
}

/// BeginHostingNetwork with the words of the capture: only the two pointers are ours.
static Reply BeginHosting(HleConsole& ds, Handle h, const GoldenCmd& g, const NetworkInfo& network) {
    static const char kPassphrase[] = "AzaharLink replay";
    return Replay(ds, h, g, false, 1, [&](std::vector<u32>& v) {
        v[3] = req::Ptr(&network);
        v[5] = req::Ptr(kPassphrase);
    });
}

static std::vector<u32> PullRequest() {
    return req::Pull(2, 0x177, 0x5DC); // the words of the capture's PullPacket lines
}

static std::pair<int, int> CountsOf(DebugLines& debug, const char* prefix) {
    return DebugLines::WordCounts(debug.Last(prefix));
}

static Mac MacOf(u8 a, u8 b, u8 c, u8 d, u8 e, u8 f) {
    return Mac{a, b, c, d, e, f};
}
static const Mac kFriendAMac = MacOf(0x02, 0x00, 0x00, 0x00, 0x00, 0x02); // node 2 of PROVA 2
static const Mac kGionkoPcMac = MacOf(0x02, 0x00, 0x00, 0x00, 0x00, 0x03); // node 3 of PROVA 2
static const Mac kThirdPcMac = MacOf(0x02, 0x00, 0x00, 0x00, 0x00, 0x04);     // made up: not in the capture

/// The network tag (vendor tag, OUI type 0x15) of the beacon the host sent last: wlancommid at +4, id8 at +8, the attribute word
/// at +10, the node counts at +16 and +17, the length of the application data at +0x33 and the data from +0x34.
static bool LastBeaconTag(SentFrames& sent, std::vector<u8>* tag) {
    const std::vector<WifiPacket> all = sent.All();
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        if (it->type != WifiPacket::Type::Beacon) {
            continue;
        }
        std::size_t pos = 12;
        const std::vector<u8>& body = it->data;
        while (pos + 2 <= body.size()) {
            const u8 id = body[pos], len = body[pos + 1];
            pos += 2;
            if (pos + len > body.size()) {
                break;
            }
            if (id == 0xDD && len >= 0x34 && std::memcmp(&body[pos], kNintendoOui, 3) == 0 && body[pos + 3] == 0x15) {
                tag->assign(body.begin() + pos, body.begin() + pos + len);
                return true;
            }
            pos += len;
        }
    }
    return false;
}
static bool LastBeaconAttributes(SentFrames& sent, u16* attributes) {
    std::vector<u8> tag;
    if (!LastBeaconTag(sent, &tag)) {
        return false;
    }
    *attributes = LoadBE16(&tag[10]);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// FFE as the host (PROVA 2)
// ---------------------------------------------------------------------------------------------------------------
static void TestFfeHostReplay() {
    std::printf("FFE host golden replay (PROVA 2): the 3DS hosts, three Azahar players join, play, one leaves and comes back, the game ends the lobby\n");
    g_rules.Reset();
    std::memset(&g_stats, 0, sizeof(g_stats));
    DebugLines debug;
    MockRoom room;
    HleConsole ds(room, 0x8D);
    SentFrames sent(ds.console);
    CountingKernel& kernel = ds.kernel;

    const NodeInfo friend_a = NodeOf(Cmd(kFfeHostCmds, "node-info-2"));
    const NodeInfo gionko_pc = NodeOf(Cmd(kFfeHostCmds, "node-info-3"));
    CHECK_EQ(friend_a.friend_code_seed, 0xf10200000c3d58e2ULL);
    CHECK_EQ(gionko_pc.friend_code_seed, 0xdb0400032d7e90b3ULL);
    AzaharClient pc1(room, kFriendAMac, friend_a);
    AzaharClient pc2(room, kGionkoPcMac, gionko_pc);
    AzaharClient pc3(room, kThirdPcMac, MakeNode(0x458f00017b21e0f5ULL, "Friend-F", 0));
    AzaharClient* const pcs[3] = {&pc1, &pc2, &pc3};
    for (AzaharClient* pc : pcs) {
        pc->Bind(kChannel);
    }

    // ---- boot: ndm:u, the two nwm::UDS sessions, the multiplayer menu ----------------------------------------------
    const Handle ndm = ds.Open("ndm:u");
    Replay(ds, ndm, Cmd(kFfeHostCmds, "ndm-override"), true);
    Replay(ds, ndm, Cmd(kFfeHostCmds, "ndm-suspend"), true);
    const Handle uds1 = ds.Open("nwm::UDS");
    const Handle uds2 = ds.Open("nwm::UDS");
    Replay(ds, ndm, Cmd(kFfeHostCmds, "ndm-enter"), true);
    Reply r = Replay(ds, uds1, Cmd(kFfeHostCmds, "initialize"), false, 2);
    const u32 status_event = r.w[3];
    CHECK(status_event != 0);
    Replay(ds, uds2, Cmd(kFfeHostCmds, "status-off"));
    CHECK(CountsOf(debug, "uds cmd 0x1b") == std::make_pair(14, 3));

    // ---- the lobby: Bind, SetProbeResponseParam, BeginHostingNetwork, the first status read, GetChannel ---------------
    r = Replay(ds, uds1, Cmd(kFfeHostCmds, "bind-2"), false, 2);
    const u32 bind2_event = r.w[3];
    Replay(ds, uds1, Cmd(kFfeHostCmds, "set-0x21"));
    const NetworkInfo hosted = HostedNetwork(kFfeHostNetwork);
    BeginHosting(ds, uds1, Cmd(kFfeHostCmds, "begin-hosting"), hosted);
    CHECK(CountsOf(debug, "uds cmd 0x1d") == std::make_pair(5, 1));
    Replay(ds, uds2, Cmd(kFfeHostCmds, "status-host-1"));
    Replay(ds, uds1, Cmd(kFfeHostCmds, "get-channel"));
    CHECK(ds.console.engine->GetConnectionStatus().max_nodes == 4);

    // ---- the three PCs join one after the other: the status the game reads and the node information ----------------
    NetworkInfo target;
    std::memset(&target, 0, sizeof(target));
    target.host_mac_address = ds.console.mac;
    target.channel = kDefaultNetworkChannel;
    const u32 events_before = kernel.Signals(status_event);
    pc1.Connect(target);
    room.WaitIdle();
    CHECK(pc1.connected());
    CHECK_EQ(pc1.node_id(), 2);
    Replay(ds, uds2, Cmd(kFfeHostCmds, "status-host-2"));
    Replay(ds, uds1, Cmd(kFfeHostCmds, "node-info-2"));
    {
        // FFE publishes the list of its players in the application data of the lobby after a join: SetApplicationData with the
        // words of the capture, the bytes the plugin's next beacon carried then. The beacon of the 3DS says exactly that,
        // with the node counts of the lobby.
        const std::vector<u8> app = FromHex(kFfeHostAppData);
        CHECK_EQ(app.size(), 148u);
        std::vector<u8> buffer(app.size() + 4, 0);
        std::memcpy(buffer.data(), app.data(), app.size());
        Replay(ds, uds1, Cmd(kFfeHostCmds, "set-app-data"), false, 99,
               [&](std::vector<u32>& v) { v[3] = req::Ptr(buffer.data()); });
        ds.console.engine->Tick(platform::NowMs() + 100000);
        std::vector<u8> tag;
        CHECK(LastBeaconTag(sent, &tag));
        CHECK(tag.size() >= 0x34 + 148);
        if (tag.size() >= 0x34 + 148) {
            CHECK_EQ(tag[0x33], 148u);
            CHECK(std::equal(app.begin(), app.end(), tag.begin() + 0x34));
            CHECK_EQ(tag[16], 2u); // the beacon of the capture: 2 nodes of 4
            CHECK_EQ(tag[17], 4u);
            CHECK_EQ(LoadBE32(&tag[4]), kFfeHostNetwork.wlancommid);
            CHECK_EQ(tag[8], kFfeHostNetwork.id8);
        }
        r = ds.Call(uds1, req::GetApplicationData(0xC9), buffer.data(), static_cast<u32>(buffer.size()));
        CHECK_EQ(r.result(), 0u);
        CHECK_EQ(r.w[2], 148u); // and the game reads back what it set
    }
    pc2.Connect(target);
    room.WaitIdle();
    CHECK(pc2.connected());
    CHECK_EQ(pc2.node_id(), 3);
    Replay(ds, uds2, Cmd(kFfeHostCmds, "status-host-3"));
    Replay(ds, uds1, Cmd(kFfeHostCmds, "node-info-3"));
    pc3.Connect(target);
    room.WaitIdle();
    CHECK(pc3.connected());
    CHECK_EQ(pc3.node_id(), 4);
    r = ds.Call(uds2, req::GetConnectionStatus());
    CHECK_EQ(r.Status().total_nodes, 4);
    CHECK_EQ(r.Status().node_bitmask, 0x000F);
    CHECK_EQ(kernel.Signals(status_event) - events_before, 3u); // one status event per player who came in
    CHECK_EQ(g_stats.drop_foreign, 0u);

    // ---- play: the PCs send the capture's payloads to the 3DS; the 3DS pulls them with the capture's request ------------
    kernel.fast_sleep = true;
    u32 pulled = 0;
    for (const GoldenFrame& f : kFfeHostPulled) {
        AzaharClient& pc = *pcs[f.src - 2];
        CHECK(pc.SendTo(f.dst, kChannel, 3, FromHex(f.hex)));
    }
    room.WaitIdle();
    for (const GoldenFrame& f : kFfeHostPulled) {
        std::vector<u8> out(kWindow, 0xA5);
        const Reply p = ds.Call(uds1, PullRequest(), out.data(), static_cast<u32>(out.size()));
        CHECK_EQ(p.w[0], 0x001400C2u);
        for (int i = 0; i < 4; ++i) {
            CHECK_EQ(p.w[1 + i], f.words[i]); // result, size, source node, descriptor: the capture's
        }
        CHECK_EQ(p.w[5], req::Ptr(out.data()));
        const std::vector<u8> expect = FromHex(f.hex);
        CHECK(p.w[2] == expect.size() && std::memcmp(out.data(), expect.data(), expect.size()) == 0);
        CHECK_EQ(out[expect.size()], 0xA5u);
        ++pulled;
    }
    {
        std::vector<u8> out(kWindow, 0xA5);
        const Reply p = ds.Call(uds1, PullRequest(), out.data(), static_cast<u32>(out.size()));
        CHECK_EQ(p.result(), 0u);
        CHECK_EQ(p.w[2], 0u); // nothing else was queued
    }
    CHECK_EQ(kernel.Signals(bind2_event), pulled);
    CHECK(CountsOf(debug, "uds PullPacket bind 2") == std::make_pair(3, 5));

    // ... and the 3DS sends the capture's SendTo requests (stack garbage in the unused bits and all)
    std::map<int, std::vector<std::vector<u8>>> expected_by_pc;
    for (const GoldenFrame& f : kFfeHostSent) {
        const std::vector<u8> payload = FromHex(f.hex);
        std::vector<u8> buffer(((payload.size() + 3) & ~3u) + 4, 0);
        std::memcpy(buffer.data(), payload.data(), payload.size());
        std::vector<u32> request = req::Make(0x17, 6, 2);
        for (int i = 0; i < 8; ++i) {
            request[1 + i] = f.words[i];
        }
        request[8] = req::Ptr(buffer.data());
        const Reply s = ds.Call(uds1, request);
        CHECK_EQ(s.w[0], 0x00170040u);
        CHECK_EQ(s.result(), 0u);
        for (int k = 0; k < 3; ++k) {
            if (f.dst == kBroadcastNetworkNodeId || f.dst == k + 2) {
                expected_by_pc[k].push_back(payload);
            }
        }
    }
    CHECK(CountsOf(debug, "uds cmd 0x17") == std::make_pair(8, 1));
    room.WaitIdle();
    for (int k = 0; k < 3; ++k) {
        const std::vector<AzaharClient::Frame> inbox = pcs[k]->TakeInbox();
        CHECK_EQ(inbox.size(), expected_by_pc[k].size());
        for (std::size_t i = 0; i < inbox.size() && i < expected_by_pc[k].size(); ++i) {
            CHECK_EQ(inbox[i].src, 1);
            CHECK_EQ(inbox[i].channel, kChannel);
            CHECK(inbox[i].payload == expected_by_pc[k][i]);
        }
    }

    // ---- the PCs talk to each other with the broadcast MAC, as FFE does: the 3DS host forwards nothing (the capture:
    // 0 forwarded of 217,083 frames) and each frame reaches its player once ----------------------------------------------
    {
        const u32 forwarded_before = g_stats.forwarded;
        const u32 other_before = g_stats.drop_not_for_me;
        for (const GoldenFrame& f : kFfeHostSent) {
            CHECK(pc1.SendTo(3, kChannel, 3, FromHex(f.hex)));
        }
        room.WaitIdle();
        CHECK_EQ(pc2.TakeInbox().size(), std::size(kFfeHostSent));
        CHECK_EQ(pc3.TakeInbox().size(), 0u);
        CHECK_EQ(g_stats.forwarded, forwarded_before);
        CHECK_EQ(g_stats.drop_not_for_me - other_before, static_cast<u32>(std::size(kFfeHostSent)));
        // a frame sent to the MAC of the other PC alone goes straight there: the 3DS does not see it at all
        const auto seen = [] { return g_stats.data_delivered + g_stats.drop_not_for_me + g_stats.drop_foreign + g_stats.forwarded; };
        const u32 seen_before = seen();
        CHECK(pc1.SendTo(3, kChannel, 0, FromHex(kFfeHostSent[0].hex)));
        room.WaitIdle();
        CHECK_EQ(pc2.TakeInbox().size(), 1u);
        CHECK_EQ(seen(), seen_before);
    }

    // ---- a PC leaves and comes back (the lid of PROVA 2): its place is free again, and a second goodbye that arrives after
    // it has come back is not its leaving again (L6) ------------------------------------------------------------------
    const u32 late_before = g_stats.deauth_late;
    pc1.Disconnect();
    room.WaitIdle();
    r = ds.Call(uds2, req::GetConnectionStatus());
    CHECK_EQ(r.Status().total_nodes, 3);
    CHECK_EQ(r.Status().node_bitmask, 0x000D); // node 2 is gone
    pc1.Connect(target);
    room.WaitIdle();
    CHECK(pc1.connected());
    CHECK_EQ(pc1.node_id(), 2); // the lowest free place: the same one
    Replay(ds, uds1, Cmd(kFfeHostCmds, "node-info-2")); // the same identity
    pc1.SendDeauthTo(ds.console.mac);                    // the late copy of its first goodbye
    room.WaitIdle();
    r = ds.Call(uds2, req::GetConnectionStatus());
    CHECK_EQ(r.Status().total_nodes, 4);
    CHECK_EQ(g_stats.deauth_late - late_before, 1u);
    CHECK(pc1.connected());
    // ... and the guard is used up by that copy: the PC that now really leaves, a moment later, is removed
    pc1.Disconnect();
    room.WaitIdle();
    r = ds.Call(uds2, req::GetConnectionStatus());
    CHECK_EQ(r.Status().total_nodes, 3);
    CHECK_EQ(g_stats.deauth_late - late_before, 1u);
    pc1.Connect(target);
    room.WaitIdle();
    CHECK(pc1.connected());
    CHECK_EQ(pc1.node_id(), 2);
    CHECK_EQ(ds.Call(uds2, req::GetConnectionStatus()).Status().total_nodes, 4);

    // ---- the game ends the lobby: Unbind(2), the other thread's Pull on the bind that is gone, Unbind(1), Bind(4), Destroy ---
    const int deauths_before = sent.CountTo(WifiPacket::Type::Deauthentication, kBroadcastMac);
    r = Replay(ds, uds1, Cmd(kFfeHostCmds, "unbind-2"), false, 3);
    CHECK_EQ(r.w[4], pulled); // the frames the bind stored: the ones it was given
    CHECK_EQ(r.w[5], 0u);     // ... and none pushed out (the capture: 501, from the other lobby)
    CHECK_EQ(kernel.Signals(bind2_event), pulled + 1);
    Replay(ds, uds2, Cmd(kFfeHostCmds, "pull-no-bind"), false, 1, [](std::vector<u32>& v) { v = PullRequest(); });
    Replay(ds, uds1, Cmd(kFfeHostCmds, "unbind-1"));
    r = Replay(ds, uds1, Cmd(kFfeHostCmds, "bind-4"), false, 2);
    const u32 bind4_event = r.w[3];
    CHECK_EQ(sent.CountTo(WifiPacket::Type::Deauthentication, kBroadcastMac), deauths_before);
    Replay(ds, uds1, Cmd(kFfeHostCmds, "destroy"));
    CHECK_EQ(sent.CountTo(WifiPacket::Type::Deauthentication, kBroadcastMac) - deauths_before, 3u); // the capture: 3 x type 4
    Replay(ds, uds2, Cmd(kFfeHostCmds, "status-after-destroy"));
    Replay(ds, uds1, Cmd(kFfeHostCmds, "unbind-4"));
    Replay(ds, uds1, Cmd(kFfeHostCmds, "unbind-3"));
    CHECK_EQ(kernel.Signals(bind4_event), 2u); // woken by Destroy and freed by Unbind
    Replay(ds, uds1, Cmd(kFfeHostCmds, "shutdown"));
    ds.Close(uds1);
    ds.Close(uds2);
    Replay(ds, ndm, Cmd(kFfeHostCmds, "ndm-leave"), true);
    ds.Close(ndm);

    CHECK_EQ(g_stats.drop_foreign, 0u); // a whole lobby of ours and not one frame taken for a stranger's
    CHECK_EQ(g_stats.foreign_shadow, 0u);
    CHECK_EQ(g_stats.ring_overwritten, 0u);
    CHECK_EQ(pc1.misrouted() + pc2.misrouted() + pc3.misrouted(), 0u);
    CHECK(g_rules.inspected() > 40);
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------------------------
// TFH as the host (first TFH trace)
// ---------------------------------------------------------------------------------------------------------------
static void TestTfhHostReplay() {
    std::printf("TFH host golden replay (first TFH trace): a lobby of 3, UpdateNetworkAttribute, the lobby ends with Unbind and Shutdown\n");
    g_rules.Reset();
    std::memset(&g_stats, 0, sizeof(g_stats));
    DebugLines debug;
    MockRoom room;
    HleConsole ds(room, 0x8D);
    SentFrames sent(ds.console);
    CountingKernel& kernel = ds.kernel;
    AzaharClient pc1(room, kFriendAMac, MakeNode(0xf10200000c3d58e2ULL, "Friend-A", 0));
    AzaharClient pc2(room, kGionkoPcMac, MakeNode(0xdb0400032d7e90b3ULL, "Gionko-PC", 0));
    for (AzaharClient* pc : {&pc1, &pc2}) {
        pc->Bind(kChannel);
    }

    const Handle ndm = ds.Open("ndm:u");
    Replay(ds, ndm, Cmd(kTfhHostCmds, "ndm-override"), true);
    Replay(ds, ndm, Cmd(kTfhHostCmds, "ndm-suspend"), true);
    const Handle uds1 = ds.Open("nwm::UDS");
    const Handle uds2 = ds.Open("nwm::UDS");
    Replay(ds, ndm, Cmd(kTfhHostCmds, "ndm-enter"), true);
    Reply r = Replay(ds, uds1, Cmd(kTfhHostCmds, "initialize"), false, 2);
    const u32 status_event = r.w[3];
    r = Replay(ds, uds1, Cmd(kTfhHostCmds, "bind-2"), false, 2);
    const u32 bind2_event = r.w[3];
    Replay(ds, uds1, Cmd(kTfhHostCmds, "set-0x21"));
    const NetworkInfo hosted = HostedNetwork(kTfhHostNetwork);
    BeginHosting(ds, uds1, Cmd(kTfhHostCmds, "begin-hosting"), hosted);
    Replay(ds, uds2, Cmd(kTfhHostCmds, "status-host-1"));
    CHECK_EQ(ds.console.engine->GetConnectionStatus().max_nodes, 3);

    NetworkInfo target;
    std::memset(&target, 0, sizeof(target));
    target.host_mac_address = ds.console.mac;
    target.channel = kDefaultNetworkChannel;
    const u32 events_before = kernel.Signals(status_event);
    pc1.Connect(target);
    room.WaitIdle();
    Replay(ds, uds2, Cmd(kTfhHostCmds, "status-host-2"));
    pc2.Connect(target);
    room.WaitIdle();
    Replay(ds, uds2, Cmd(kTfhHostCmds, "status-host-3"));
    CHECK_EQ(kernel.Signals(status_event) - events_before, 2u);

    kernel.fast_sleep = true;
    AzaharClient* const pcs[2] = {&pc1, &pc2};
    u32 pulled = 0;
    for (const GoldenFrame& f : kTfhHostPulled) {
        CHECK(pcs[f.src - 2]->SendTo(f.dst, kChannel, 3, FromHex(f.hex))); // TFH sends with flags 3 (broadcast) always
    }
    room.WaitIdle();
    for (const GoldenFrame& f : kTfhHostPulled) {
        std::vector<u8> out(kWindow, 0xA5);
        const Reply p = ds.Call(uds1, PullRequest(), out.data(), static_cast<u32>(out.size()));
        for (int i = 0; i < 4; ++i) {
            CHECK_EQ(p.w[1 + i], f.words[i]);
        }
        const std::vector<u8> expect = FromHex(f.hex);
        CHECK(p.w[2] == expect.size() && std::memcmp(out.data(), expect.data(), expect.size()) == 0);
        ++pulled;
    }
    CHECK_EQ(kernel.Signals(bind2_event), pulled);
    {
        // what a PC sends to everybody (node 0xFFFF) reaches the other PC as well, with the broadcast MAC it travels on
        std::size_t everybody_from_1 = 0, everybody_from_2 = 0;
        for (const GoldenFrame& f : kTfhHostPulled) {
            if (f.dst == kBroadcastNetworkNodeId) {
                (f.src == 2 ? everybody_from_1 : everybody_from_2) += 1;
            }
        }
        CHECK(everybody_from_1 > 0);
        CHECK_EQ(pc2.TakeInbox().size(), everybody_from_1);
        CHECK_EQ(pc1.TakeInbox().size(), everybody_from_2);
    }
    std::size_t sent_to_everybody = 0;
    for (const GoldenFrame& f : kTfhHostSent) {
        const std::vector<u8> payload = FromHex(f.hex);
        std::vector<u8> buffer(((payload.size() + 3) & ~3u) + 4, 0);
        std::memcpy(buffer.data(), payload.data(), payload.size());
        std::vector<u32> request = req::Make(0x17, 6, 2);
        for (int i = 0; i < 8; ++i) {
            request[1 + i] = f.words[i];
        }
        request[8] = req::Ptr(buffer.data());
        const Reply s = ds.Call(uds1, request);
        CHECK_EQ(s.result(), 0u);
        sent_to_everybody += f.dst == kBroadcastNetworkNodeId;
    }
    room.WaitIdle();
    const std::size_t to_pc1 = pc1.TakeInbox().size();
    const std::size_t to_pc2 = pc2.TakeInbox().size();
    std::size_t for_pc1 = sent_to_everybody, for_pc2 = sent_to_everybody;
    for (const GoldenFrame& f : kTfhHostSent) {
        for_pc1 += f.dst == 2;
        for_pc2 += f.dst == 3;
    }
    CHECK_EQ(to_pc1, for_pc1);
    CHECK_EQ(to_pc2, for_pc2);

    // UpdateNetworkAttribute (the one command the capture has no words of: its first hardware trace changed the beacon's
    // attribute word 0x8000 -> 0x8006 at 143 s and back later): the beacon the 3DS sends next says so
    u16 attributes = 0;
    ds.console.engine->Tick(platform::NowMs() + 100000);
    CHECK(LastBeaconAttributes(sent, &attributes));
    CHECK_EQ(attributes, 0x8000);
    r = ds.Call(uds1, req::Words(0x07, 2, 0x0006, 1));
    CHECK_EQ(r.w[0], 0x00070040u);
    CHECK_EQ(r.result(), 0u);
    ds.console.engine->Tick(platform::NowMs() + 200000);
    CHECK(LastBeaconAttributes(sent, &attributes));
    CHECK_EQ(attributes, 0x8006);
    r = ds.Call(uds1, req::Words(0x07, 2, 0x0006, 0));
    CHECK_EQ(r.result(), 0u);
    ds.console.engine->Tick(platform::NowMs() + 300000);
    CHECK(LastBeaconAttributes(sent, &attributes));
    CHECK_EQ(attributes, 0x8000);

    // the end: no DestroyNetwork - Unbind twice and Shutdown close the lobby, and the engine says goodbye (3 Deauthentications)
    const int deauths_before = sent.CountTo(WifiPacket::Type::Deauthentication, kBroadcastMac);
    r = Replay(ds, uds1, Cmd(kTfhHostCmds, "unbind-2"), false, 3);
    CHECK_EQ(r.w[4], pulled);
    CHECK_EQ(r.w[5], 0u);
    Replay(ds, uds1, Cmd(kTfhHostCmds, "unbind-1"));
    CHECK_EQ(sent.CountTo(WifiPacket::Type::Deauthentication, kBroadcastMac), deauths_before);
    Replay(ds, uds1, Cmd(kTfhHostCmds, "shutdown"));
    CHECK_EQ(sent.CountTo(WifiPacket::Type::Deauthentication, kBroadcastMac) - deauths_before, 3u);
    CHECK_EQ(ds.console.engine->GetConnectionStatus().status, 0u); // the service is off: not even "not connected" (3)
    ds.Close(uds1);
    ds.Close(uds2);
    Replay(ds, ndm, Cmd(kTfhHostCmds, "ndm-leave"), true);
    ds.Close(ndm);

    CHECK_EQ(g_stats.drop_foreign, 0u);
    CHECK_EQ(g_stats.foreign_shadow, 0u);
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------------------------
// The lid episode of PROVA 2 (L4): a second lobby of the same game in the room
// ---------------------------------------------------------------------------------------------------------------
namespace {

std::vector<u8> Filler(u16 src, u16 dst, std::size_t size, u32 serial) {
    std::vector<u8> v(size);
    for (std::size_t i = 0; i < size; ++i) {
        v[i] = static_cast<u8>(serial * 13 + i * 7 + src);
    }
    if (size >= 4) {
        v[0] = 0x02; // the bytes FFE starts with: 02 01 <source> <destination>
        v[1] = 0x01;
        v[2] = static_cast<u8>(src);
        v[3] = static_cast<u8>(dst);
    }
    return v;
}

} // namespace

static void TestOtherLobbyEpisode(bool with_filter_check) {
    std::printf("lid episode (PROVA 2): 4 of 4, the PCs leave, they play on in a lobby of their own (%zu frames), they come back\n",
                std::size(kOtherLobby));
    g_rules.Reset();
    std::memset(&g_stats, 0, sizeof(g_stats));
    MockRoom room;
    HleConsole ds(room, 0x8D);
    SentFrames sent(ds.console);
    CountingKernel& kernel = ds.kernel;
    AzaharClient pc1(room, kFriendAMac, MakeNode(0xf10200000c3d58e2ULL, "Friend-A", 0));
    AzaharClient pc2(room, kGionkoPcMac, MakeNode(0xdb0400032d7e90b3ULL, "Gionko-PC", 0));
    AzaharClient pc3(room, kThirdPcMac, MakeNode(0x458f00017b21e0f5ULL, "Friend-F", 0));
    AzaharClient* const pcs[3] = {&pc1, &pc2, &pc3};
    for (AzaharClient* pc : pcs) {
        pc->Bind(kChannel);
    }

    const Handle uds = ds.Open("nwm::UDS");
    CHECK_EQ(ds.Call(uds, req::Initialize(MakeNode(0x371100020a5cc011ULL, "Gionko", 0), kTestSharedMemory, 0x400)).result(), 0u);
    Reply r = ds.Call(uds, req::Bind(2, 0x2E30, kChannel, 0xFFFF));
    CHECK_EQ(r.result(), 0u);
    const u32 bind_event = r.w[3];
    const NetworkInfo hosted = HostedNetwork(kFfeHostNetwork);
    CHECK_EQ(ds.Call(uds, req::BeginHosting(&hosted)).result(), 0u);
    NetworkInfo target;
    std::memset(&target, 0, sizeof(target));
    target.host_mac_address = ds.console.mac;
    target.channel = kDefaultNetworkChannel;
    for (AzaharClient* pc : pcs) {
        pc->Connect(target);
        room.WaitIdle();
        CHECK(pc->connected());
    }
    CHECK_EQ(ds.Call(uds, req::GetConnectionStatus()).Status().total_nodes, 4);
    kernel.fast_sleep = true;

    const auto pull_all = [&]() {
        u32 n = 0;
        for (;;) {
            std::vector<u8> out(kWindow, 0xA5);
            const Reply p = ds.Call(uds, PullRequest(), out.data(), static_cast<u32>(out.size()));
            CHECK_EQ(p.result(), 0u);
            if (p.w[2] == 0) {
                return n;
            }
            ++n;
        }
    };
    // plays a row of the trace: a PC (the node it names) sends the frame with the broadcast MAC, as FFE does
    const auto play = [&](const TraceFrame& f, u32 serial) {
        AzaharClient& pc = *pcs[f.src - 2];
        return pc.SendTo(f.dst, kChannel, 3, Filler(f.src, f.dst, f.size, serial));
    };

    // ---- 1. the lobby of 4 as it was before the lid: what is for node 1 is delivered, the rest is not the 3DS's ----------
    u32 serial = 0, for_us = 0, for_others = 0;
    for (const TraceFrame& f : kLobbyBeforeLid) {
        CHECK(play(f, serial++));
        (f.kind == 'R' ? for_us : for_others) += 1;
        if (serial % 40 == 0) { // the game pulls as it goes
            room.WaitIdle();
            pull_all();
        }
    }
    room.WaitIdle();
    const u32 left = pull_all();
    CHECK_EQ(g_stats.data_delivered, for_us);       // (the pulls are counted by the frames that got to the game)
    CHECK(left <= 40);
    CHECK_EQ(g_stats.drop_foreign, 0u);             // nothing a member sent was taken for a stranger's
    CHECK_EQ(g_stats.foreign_shadow, 0u);
    CHECK_EQ(g_stats.forwarded, 0u);                // (FFE's frames carry the broadcast MAC: a host does not pass them on)
    CHECK_EQ(g_stats.drop_not_for_me, for_others);
    CHECK_EQ(g_stats.ring_overwritten, 0u);

    // ---- 2. the lid: the three PCs give up and leave (the capture: Deauthentications 4 s after the 3DS went dark) ---------
    for (AzaharClient* pc : pcs) {
        pc->Disconnect();
    }
    room.WaitIdle();
    ConnectionStatus st = ds.Call(uds, req::GetConnectionStatus()).Status();
    CHECK_EQ(st.total_nodes, 1);
    CHECK_EQ(st.node_bitmask, 1);
    const u32 events = kernel.Signals(bind_event);

    // ---- 3. the PCs make a lobby of their own in the same room. Its host is node 1, a guest is node 2: the node ids are
    // ours. The frames are the ones the 3DS logged then (all "2>1": the guest to its host), from MACs that were nodes of ours
    // a moment ago. 0.1.15 took them for frames of its own lobby: the game's ring overflowed (501 pushed out). --------------
    std::memset(&g_stats, 0, sizeof(g_stats));
    u32 foreign = 0;
    for (const TraceFrame& f : kOtherLobby) {
        pc1.SendInOtherLobby(f.src, f.dst, kChannel, Filler(f.src, f.dst, f.size, 5000 + foreign));
        ++foreign;
    }
    room.WaitIdle();
    CHECK(foreign > 200);
    CHECK_EQ(pull_all(), 0u);                        // nothing reached the game
    CHECK_EQ(kernel.Signals(bind_event), events);    // ... and it was not woken for it
    CHECK_EQ(g_stats.data_delivered, 0u);
    CHECK_EQ(g_stats.ring_overwritten, 0u);          // the "frames lost" warning had nothing to count
    CHECK_EQ(g_stats.drop_foreign, foreign);         // every one of them was refused for what it is
    CHECK_EQ(g_stats.forwarded, 0u);
    st = ds.Call(uds, req::GetConnectionStatus()).Status();
    CHECK_EQ(st.total_nodes, 1);
    CHECK_EQ(st.status, static_cast<u32>(NetworkStatus::ConnectedAsHost));

    // ---- 4. the guest of that lobby comes back to ours (2027.7 s) and a second PC after it (2041.1 s): from the moment a PC
    // is admitted again its frames are delivered; a PC that has not come back is still a stranger ---------------------------
    std::memset(&g_stats, 0, sizeof(g_stats));
    bool joined2 = false, joined3 = false;
    u32 delivered = 0, others = 0, strangers = 0;
    serial = 9000;
    for (const TraceFrame& f : kLobbyAfterLid) {
        if (!joined2) {
            pc1.Connect(target);
            room.WaitIdle();
            CHECK(pc1.connected());
            CHECK_EQ(pc1.node_id(), 2);
            joined2 = true;
        }
        if (!joined3 && (f.src == 3 || f.dst == 3)) {
            // (Gionko-PC was sending in the other lobby until here: those frames are a stranger's)
            pc2.SendInOtherLobby(3, 1, kChannel, Filler(3, 1, 50, 777));
            room.WaitIdle();
            ++strangers;
            pc2.Connect(target);
            room.WaitIdle();
            CHECK(pc2.connected());
            CHECK_EQ(pc2.node_id(), 3);
            joined3 = true;
        }
        AzaharClient& pc = f.src == 2 ? pc1 : pc2;
        CHECK(pc.SendTo(f.dst, kChannel, 3, Filler(f.src, f.dst, f.size, serial++)));
        (f.kind == 'R' ? delivered : others) += 1;
        if (serial % 40 == 0) {
            room.WaitIdle();
            pull_all();
        }
    }
    room.WaitIdle();
    pull_all();
    CHECK(joined2 && joined3);
    CHECK_EQ(g_stats.data_delivered, delivered);
    CHECK_EQ(g_stats.drop_not_for_me, others);
    CHECK_EQ(g_stats.drop_foreign, strangers);
    CHECK_EQ(g_stats.ring_overwritten, 0u);
    CHECK_EQ(ds.Call(uds, req::GetConnectionStatus()).Status().total_nodes, 3);
    if (with_filter_check) {
        CHECK(delivered > 100);
    }
    ds.Close(uds);
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------------------------
// The client stretch of PROVA 1
// ---------------------------------------------------------------------------------------------------------------
static void TestClientStretch() {
    std::printf("client stretch (PROVA 1): the 3DS as a client of a PC host, %zu real frames; the foreign-lobby filter counts nothing, on or off\n",
                std::size(kClientStretch));
    for (const bool enforced : {false, true}) {
        g_rules.Reset();
        std::memset(&g_stats, 0, sizeof(g_stats));
        MockRoom room;
        NetworkInfo net = MakeNetworkInfo(4, 148);
        StoreBE32(net.wlan_comm_id_be, 0x0016ED10);
        net.id = 1;
        AzaharHost host(room, 1, net, MakeNode(0x1111, "Gionko-PC", 0));
        HleConsole ds(room, 0x8D);
        ds.console.engine->SetClientForeignFilterEnforced(enforced);
        const Handle uds = ds.Open("nwm::UDS");
        CHECK_EQ(ds.Call(uds, req::Initialize(MakeNode(0x371100020a5cc011ULL, "Gionko", 0), kTestSharedMemory, 0x400)).result(), 0u);
        CHECK_EQ(ds.Call(uds, req::Bind(2, 0x2E30, kChannel, 0xFFFF)).result(), 0u);
        NetworkInfo target = host.network();
        CHECK_EQ(ds.Call(uds, req::Connect(1, &target)).result(), 0u);
        room.WaitIdle();
        const u16 me = ds.Call(uds, req::GetConnectionStatus()).Status().network_node_id;
        CHECK_EQ(me, 2);
        ds.kernel.fast_sleep = true;
        u32 serial = 0, expected = 0, pulled = 0;
        std::vector<AzaharHost::Frame> at_host;
        for (const TraceFrame& f : kClientStretch) {
            if (f.kind == 'T') {
                const std::vector<u8> payload = Filler(me, f.dst, f.size, serial++);
                const Reply s = ds.Call(uds, req::SendTo(f.dst, kChannel, 2, payload.data(), static_cast<u32>(payload.size())));
                CHECK_EQ(s.result(), 0u);
            } else if (f.kind == 'R') {
                CHECK(host.SendData(f.dst, kChannel, 2, Filler(f.src, f.dst, f.size, serial++)));
                ++expected;
            }
            if (serial % 30 == 0) {
                room.WaitIdle();
                for (;;) {
                    std::vector<u8> out(kWindow, 0xA5);
                    const Reply p = ds.Call(uds, PullRequest(), out.data(), static_cast<u32>(out.size()));
                    if (p.w[2] == 0) {
                        break;
                    }
                    ++pulled;
                }
            }
        }
        room.WaitIdle();
        for (;;) {
            std::vector<u8> out(kWindow, 0xA5);
            const Reply p = ds.Call(uds, PullRequest(), out.data(), static_cast<u32>(out.size()));
            if (p.w[2] == 0) {
                break;
            }
            ++pulled;
        }
        CHECK_EQ(pulled, expected);                      // every frame of the host reached the game, once
        CHECK_EQ(g_stats.drop_foreign, 0u);              // ... the filter, on, took none of them for a stranger's
        CHECK_EQ(g_stats.foreign_shadow, 0u);            // ... and, off, would not have counted one
        CHECK_EQ(g_stats.ring_overwritten, 0u);
        CHECK(expected > 100);
        ds.Close(uds);
        CHECK_RULES();
    }
}

int main() {
    LogOn log_on; // (the replays read the lines of the capture)
    SetLogSink([](const char*) {});
    Watchdog dog(180);
    dog.Step("FFE host");
    TestFfeHostReplay();
    dog.Step("TFH host");
    TestTfhHostReplay();
    dog.Step("the lid episode");
    TestOtherLobbyEpisode(true);
    dog.Step("the client stretch");
    TestClientStretch();
    dog.Step("");
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
