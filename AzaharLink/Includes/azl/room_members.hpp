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
    bool plays = true; ///< false: it told the room it plays nothing (RoomMemberPlays)
};

/// Whether a member of the room plays something: an empty title and id 0 is what a member tells the room when it plays nothing
/// (Azahar's "is not playing", sent when its emulation is stopped; a console with the plugin always says its game). A member that
/// stays in the room but plays nothing has left the game's session: an emulator that is closed says no goodbye on the air, and
/// the host would wait for its frames until the game itself gives up (Kirby Battle Royale, 2026-10-28: 30 s). So the engine is
/// given the members that play (Plugin::OnRoomInfo), and the one that stops goes the way of one that left the room.
inline bool RoomMemberPlays(const std::string& game_name, u64 game_id) {
    return !game_name.empty() || game_id != 0;
}

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

/// The lines for the players that played in `before` and play nothing in `now` (told apart by their MAC): "room: 'Friend-A-PC'
/// [Kirby Battle Royale] stopped playing (still in the room, MAC 02:00:00:00:00:02): the game's session drops it". A player that
/// was not in `before`, one that left the room, and one that did not play before get no line here.
inline std::vector<std::string> RoomMemberStoppedLines(const std::vector<RoomMemberRow>& before, const std::vector<RoomMemberRow>& now) {
    std::vector<std::string> lines;
    for (const RoomMemberRow& m : before) {
        if (!m.plays) {
            continue;
        }
        for (const RoomMemberRow& n : now) {
            if (n.mac == m.mac && !n.plays) {
                char mac[24];
                std::snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", m.mac[0], m.mac[1], m.mac[2], m.mac[3], m.mac[4],
                              m.mac[5]);
                lines.push_back("room: '" + n.nickname + "'" + (m.game.empty() ? std::string() : " [" + m.game + "]") +
                                " stopped playing (still in the room, MAC " + mac + "): the game's session drops it");
                break;
            }
        }
    }
    return lines;
}

} // namespace azl
