// room_routing_probe: asks the REAL azahar-room server who receives a WifiPacket for each destination MAC, the
// fact the twin filter (uds_dedupe.hpp) stands on: a frame sent to the broadcast MAC reaches every other member
// once, one sent to a member's MAC reaches only that member, the transmitter field is left alone.
//   room_routing_probe <host> <port>
// Four members join; A sends Data frames with (1) the broadcast MAC, (2) B's MAC, (3) an unknown MAC and (4) a forged
// transmitter. Prints and checks the copies each member gets. Run by Tests/integration.sh.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "azl/room_client.hpp"
#include "azl/uds_frames.hpp"
#include "support.hpp"

using namespace azl;

struct Member {
    explicit Member(const std::string& nick, const std::string& host, u16 port) {
        cfg.host = host;
        cfg.port = port;
        cfg.nickname = nick;
        cfg.console_id_hash = "routing-" + nick;
        cfg.game_name = "routing probe";
        cfg.game_id = 0x0004000000164800ULL;
        client.SetWifiCallback([this](const WifiPacket& p) {
            std::lock_guard<std::mutex> l(m);
            if (p.type == WifiPacket::Type::Data && p.data.size() > 8 && p.data[8] == 0x5A) {
                got.push_back(p);
            }
        });
    }
    bool Join() {
        client.Start(cfg);
        for (int i = 0; i < 800 && !client.joined(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return client.joined();
    }
    std::vector<WifiPacket> Take() {
        std::lock_guard<std::mutex> l(m);
        auto v = std::move(got);
        got.clear();
        return v;
    }
    RoomClient::Config cfg;
    RoomClient client;
    std::mutex m;
    std::vector<WifiPacket> got;
};

static WifiPacket Frame(const Mac& tx, const Mac& dst, u8 tag) {
    WifiPacket p;
    p.type = WifiPacket::Type::Data;
    p.channel = 11;
    p.transmitter_address = tx;
    p.destination_address = dst;
    p.data = {0xAA, 0xAA, 0x03, 0, 0, 0, 0x87, 0x6D, 0x5A, tag}; // byte 8 = marker so other frames are ignored
    return p;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        return 2;
    }
    const std::string host = argv[1];
    const u16 port = static_cast<u16>(std::atoi(argv[2]));
    Member A("RouteA_x", host, port), B("RouteB_x", host, port), C("RouteC_x", host, port), D("RouteD_x", host, port);
    CHECK(A.Join() && B.Join() && C.Join() && D.Join());
    const Mac ma = A.client.mac(), mb = B.client.mac(), mc = C.client.mac();
    Member* all[4] = {&A, &B, &C, &D};
    auto count = [&](const char* what) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        int n[4];
        std::vector<WifiPacket> first[4];
        for (int i = 0; i < 4; ++i) {
            first[i] = all[i]->Take();
            n[i] = static_cast<int>(first[i].size());
        }
        std::printf("%-58s A=%d B=%d C=%d D=%d\n", what, n[0], n[1], n[2], n[3]);
        return std::vector<int>(n, n + 4);
    };
    // (1) broadcast MAC: everybody except the sender
    A.client.SendWifiPacket(Frame(ma, kBroadcastMac, 1));
    auto r = count("A -> broadcast MAC");
    CHECK(r == std::vector<int>({0, 1, 1, 1}));
    // (2) unicast to B: only B
    A.client.SendWifiPacket(Frame(ma, mb, 2));
    r = count("A -> B's MAC");
    CHECK(r == std::vector<int>({0, 1, 0, 0}));
    // (3) unknown MAC: nobody, and the sender stays connected
    Mac nobody = {0x00, 0x1F, 0x32, 0xFE, 0xED, 0x01};
    A.client.SendWifiPacket(Frame(ma, nobody, 3));
    r = count("A -> unknown MAC");
    CHECK(r == std::vector<int>({0, 0, 0, 0}));
    CHECK(A.client.joined());
    // (4) the room does not touch the transmitter field (A claims to be C)
    A.client.SendWifiPacket(Frame(mc, mb, 4));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    auto got = B.Take();
    CHECK_EQ(got.size(), std::size_t(1));
    if (!got.empty()) {
        CHECK(got[0].transmitter_address == mc);
        CHECK(got[0].destination_address == mb);
        std::printf("forged transmitter (C's MAC) arrives unchanged at B: %s\n", got[0].transmitter_address == mc ? "yes" : "NO");
    }
    // (5) two broadcasts back to back arrive in order at a single receiver
    A.client.SendWifiPacket(Frame(ma, kBroadcastMac, 10));
    A.client.SendWifiPacket(Frame(ma, kBroadcastMac, 11));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    got = D.Take();
    CHECK_EQ(got.size(), std::size_t(2));
    if (got.size() == 2) {
        CHECK_EQ(got[0].data[9], 10);
        CHECK_EQ(got[1].data[9], 11);
    }
    for (Member* m : all) {
        m->client.Stop();
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
