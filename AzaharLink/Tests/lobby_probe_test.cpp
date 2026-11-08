// lobby_probe_test: ConfigApp/source/lobby_probe.cpp against real azahar-room containers (Tests/integration.sh): the rooms of "Public lobbies"
// are asked without joining. argv: <open room host> <port> <room with password "secret", 2 slots> <port>.
// What must hold: the ping, the room's information (name, slots, members and what they play), open or password, our password right or wrong,
// a full room, the protocol version, a room that is not there; and that the members in the room see nobody come or go.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "azl/room_proto.hpp"
#include "enet/enet.h"
#include "lobby_probe.hpp"

using namespace lobbies;
namespace room = azl::room;

static int g_failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

// A member of the room: a real join, and a count of what the room tells it while the pass runs.
struct RoomMember {
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    int status_messages = 0, room_infos = 0;
    bool joined = false;

    bool Join(const char* address, std::uint16_t port, const char* nick, const char* password, std::uint8_t mac_end) {
        host = enet_host_create(nullptr, 1, room::kNumChannels, 0, 0);
        ENetAddress a{};
        enet_address_set_host(&a, address);
        a.port = port;
        peer = enet_host_connect(host, &a, room::kNumChannels, 0);
        azl::Mac mac = {0x40, 0, 0, 0, 0, mac_end};
        const std::vector<azl::u8> join = room::EncodeJoinRequest(nick, std::string("member-") + nick, mac, password, "");
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < until && !joined) {
            ENetEvent e;
            if (enet_host_service(host, &e, 10) > 0) {
                if (e.type == ENET_EVENT_TYPE_CONNECT) {
                    enet_peer_send(peer, 0, enet_packet_create(join.data(), join.size(), ENET_PACKET_FLAG_RELIABLE));
                } else if (e.type == ENET_EVENT_TYPE_RECEIVE) {
                    joined = joined || e.packet->data[0] == room::IdJoinSuccess || e.packet->data[0] == room::IdJoinSuccessAsMod;
                    enet_packet_destroy(e.packet);
                }
            }
        }
        if (joined) {
            const std::vector<azl::u8> game = room::EncodeSetGameInfo("Monster Hunter 4 Ultimate", 0x0004000000126100ull);
            enet_peer_send(peer, 0, enet_packet_create(game.data(), game.size(), ENET_PACKET_FLAG_RELIABLE));
            Serve(300);
            status_messages = room_infos = 0;
        }
        return joined;
    }
    void Serve(int ms) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            ENetEvent e;
            while (enet_host_service(host, &e, 1) > 0) {
                if (e.type == ENET_EVENT_TYPE_RECEIVE) {
                    status_messages += e.packet->data[0] == room::IdStatusMessage ? 1 : 0;
                    room_infos += e.packet->data[0] == room::IdRoomInformation ? 1 : 0;
                    enet_packet_destroy(e.packet);
                }
            }
        }
    }
    void Leave() {
        if (host) {
            enet_peer_disconnect_now(peer, 0);
            enet_host_destroy(host);
            host = nullptr;
        }
    }
};

static void RunPass(ProbePass& pass, RoomMember* a, RoomMember* b, int max_ms) {
    CHECK(pass.Begin());
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_ms);
    while (pass.Step(5) && std::chrono::steady_clock::now() < until) {
        pass.TakeChanged();
        if (a) {
            a->Serve(1);
        }
        if (b) {
            b->Serve(1);
        }
    }
    CHECK(std::chrono::steady_clock::now() < until);
}

int main(int argc, char** argv) {
    if (argc < 5) {
        std::printf("usage: lobby_probe_test <open room> <port> <locked room> <port>\n");
        return 2;
    }
    const std::string open_host = argv[1], locked_host = argv[3];
    const std::uint16_t open_port = static_cast<std::uint16_t>(std::atoi(argv[2])), locked_port = static_cast<std::uint16_t>(std::atoi(argv[4]));
    enet_initialize();

    RoomMember in_open;
    CHECK(in_open.Join(open_host.c_str(), open_port, "MemberA", "", 1));

    // 1. The open room, the locked room with no password, with the right one and with a wrong one, a port where nothing answers, a name that
    // does not resolve.
    {
        PassOptions o;
        ProbePass pass({{open_host, open_port, ""},
                        {locked_host, locked_port, ""},
                        {locked_host, locked_port, "secret"},
                        {locked_host, locked_port, "wrong"},
                        {open_host, 1, ""},
                        {"no-such-room.invalid", 24872, ""}},
                       o);
        const auto start = std::chrono::steady_clock::now();
        RunPass(pass, &in_open, nullptr, 8000);
        const long took = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        std::printf("pass of 6 rooms: %ld ms\n", took);
        CHECK(took < 4000); // (all at once: the slowest is the 2.5 s of the room that is not there)

        const Probe& open = pass.result(0);
        CHECK(open.state == State::Online && open.ping_ms >= 0 && open.ping_ms < 500);
        CHECK(open.has_info && open.slots == 8 && open.members.size() == 1 && open.name == "AzaharLink test");
        CHECK(open.members.size() == 1 && open.members[0].game_id == 0x0004000000126100ull && open.members[0].game == "Monster Hunter 4 Ultimate");
        CHECK(open.lock == Lock::Open && open.version == room::kNetworkVersion && !open.full);

        const Probe& locked = pass.result(1);
        CHECK(locked.state == State::Online && locked.has_info && locked.slots == 2 && locked.members.empty());
        CHECK(locked.lock == Lock::Password && !locked.secret_ok && locked.version == 0);
        const Probe& right = pass.result(2);
        CHECK(right.lock == Lock::Password && right.secret_ok && right.version == room::kNetworkVersion);
        const Probe& wrong = pass.result(3);
        CHECK(wrong.lock == Lock::Password && !wrong.secret_ok);
        CHECK(pass.result(4).state == State::Offline && pass.result(4).ping_ms < 0);
        CHECK(pass.result(5).state == State::Offline);

        // nobody joined or left in the eyes of the member: no status message, and the room's information came only for the asking
        in_open.Serve(500);
        std::printf("the member saw %d status messages and %d room informations during the pass\n", in_open.status_messages, in_open.room_infos);
        CHECK(in_open.status_messages == 0);
        CHECK(in_open.room_infos >= 1 && in_open.room_infos <= 2);
    }

    // 2. The same room asked twice in a row: the second pass sees the same, and the member still sees nobody.
    {
        ProbePass again({{open_host, open_port, ""}}, PassOptions());
        RunPass(again, &in_open, nullptr, 4000);
        CHECK(again.result(0).state == State::Online && again.result(0).members.size() == 1);
        in_open.Serve(300);
        CHECK(in_open.status_messages == 0);
    }

    // 3. A full room (2 slots, 2 members): full, its information too (the room keeps one slot more for this answer).
    {
        RoomMember one, two;
        CHECK(one.Join(locked_host.c_str(), locked_port, "MemberB", "secret", 2));
        CHECK(two.Join(locked_host.c_str(), locked_port, "MemberC", "secret", 3));
        one.Serve(300);
        one.status_messages = 0; // (MemberC's join is not the pass)
        ProbePass full({{locked_host, locked_port, ""}}, PassOptions());
        RunPass(full, &one, &two, 4000);
        const Probe& p = full.result(0);
        CHECK(p.state == State::Online && p.full && IsFull(p));
        CHECK(p.has_info && p.members.size() == 2);
        one.Serve(300);
        CHECK(one.status_messages == 0);
        one.Leave();
        two.Leave();
    }

    // 4. Many rooms at once through 8 peers: 40 asks of the same two rooms end, and none is left asking.
    {
        std::vector<Target> many;
        for (int i = 0; i < 40; ++i) {
            many.push_back({i % 2 ? locked_host : open_host, i % 2 ? locked_port : open_port, ""});
        }
        ProbePass pass(many, PassOptions());
        const auto start = std::chrono::steady_clock::now();
        RunPass(pass, &in_open, nullptr, 15000);
        int online = 0;
        for (std::size_t i = 0; i < pass.size(); ++i) {
            online += pass.result(i).state == State::Online ? 1 : 0;
        }
        std::printf("40 asks, 8 at once: %d online in %ld ms\n", online,
                    static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()));
        CHECK(online == 40);
        in_open.Serve(300);
        CHECK(in_open.status_messages == 0);
    }

    in_open.Leave();
    enet_deinitialize();
    std::printf("lobby_probe_test: %s (%d failure%s)\n", g_failures ? "FAILED" : "ok", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
