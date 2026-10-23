// room_probe: joins an Azahar room, prints what the room reports, and leaves.
//   room_probe <host> [port] [nickname] [seconds-to-stay]
// Handy to check that a room is reachable and speaks network version 4 before going to the 3DS.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "azl/room_client.hpp"

using namespace azl;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <host> [port] [nickname] [seconds]\n", argv[0]);
        return 2;
    }
    RoomClient::Config config;
    config.host = argv[1];
    config.port = argc > 2 ? static_cast<u16>(std::atoi(argv[2])) : room::kDefaultPort;
    config.nickname = argc > 3 ? argv[3] : "AzlProbe";
    const int stay = argc > 4 ? std::atoi(argv[4]) : 1;
    config.console_id_hash = "azl-probe-" + config.nickname;
    config.game_name = "AzaharLink probe";
    config.game_id = 0;

    RoomClient client;
    client.Start(config);
    for (int i = 0; i < 1000; ++i) {
        if (client.joined() ||
            (client.state() == RoomClient::State::Idle && client.error() != RoomClient::Error::None)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!client.joined()) {
        std::printf("NOT JOINED: %s (host %s:%u)\n", RoomClient::ErrorName(client.error()),
                    config.host.c_str(), config.port);
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(stay));
    const room::RoomInfo info = client.room_info();
    const Mac mac = client.mac();
    std::printf("JOINED room \"%s\" (%s)\n", info.name.c_str(), info.description.c_str());
    std::printf("  slots: %u, preferred game: \"%s\", host user: \"%s\"\n", info.member_slots,
                info.preferred_game.c_str(), info.host_username.c_str());
    std::printf("  my MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4],
                mac[5]);
    std::printf("  members (%zu):\n", info.members.size());
    for (const auto& m : info.members) {
        std::printf("    - %s  [%s]  %02X:%02X:%02X:%02X:%02X:%02X\n", m.nickname.c_str(),
                    m.game_name.empty() ? "no game" : m.game_name.c_str(), m.mac[0], m.mac[1],
                    m.mac[2], m.mac[3], m.mac[4], m.mac[5]);
    }
    client.Stop();
    return 0;
}
