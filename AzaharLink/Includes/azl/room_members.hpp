// Who came into the room and who left it, as lines of the log. Kept out of the plugin (and free of the 3DS) so that the tests
// run the code the plugin runs: Plugin::NoteRoomMembers keeps the list the room gave last time and writes what this list
// has changed.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "azl/common.hpp"

namespace azl {

/// What the log needs of a player the room lists.
struct RoomMemberRow {
    Mac mac{};
    std::string nickname;
    std::string game; ///< the title the player says it plays (empty: the room has not been told yet)
};

/// The lines for the players that `now` has and `before` does not ("joined the room") and then those of `before` that `now` has
/// not ("left the room"), told apart by their MAC and in the order of the lists, each with how many players the room has now out
/// of `slots`: "room: 'Friend-A' [FINAL FANTASY EXPLORERS] joined the room (MAC 00:1F:32:AA:BB:CC): 3/8 players". The MAC is the
/// one the game's frames reach the other consoles by (the node-to-MAC list of the engine, "uds: nodes (...)", has the same).
inline std::vector<std::string> RoomMemberLines(const std::vector<RoomMemberRow>& before, const std::vector<RoomMemberRow>& now,
                                                u32 slots) {
    const auto has = [](const std::vector<RoomMemberRow>& list, const Mac& mac) {
        for (const RoomMemberRow& m : list) {
            if (m.mac == mac) {
                return true;
            }
        }
        return false;
    };
    std::vector<std::string> lines;
    const auto line = [&](const RoomMemberRow& m, const char* what) {
        char mac[24];
        std::snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", m.mac[0], m.mac[1], m.mac[2], m.mac[3], m.mac[4],
                      m.mac[5]);
        char count[48];
        std::snprintf(count, sizeof(count), "%lu/%lu", static_cast<unsigned long>(now.size()), static_cast<unsigned long>(slots));
        lines.push_back("room: '" + m.nickname + "'" + (m.game.empty() ? std::string() : " [" + m.game + "]") + " " + what +
                        " the room (MAC " + mac + "): " + count + " players");
    };
    for (const RoomMemberRow& m : now) {
        if (!has(before, m.mac)) {
            line(m, "joined");
        }
    }
    for (const RoomMemberRow& m : before) {
        if (!has(now, m.mac)) {
            line(m, "left");
        }
    }
    return lines;
}

} // namespace azl
