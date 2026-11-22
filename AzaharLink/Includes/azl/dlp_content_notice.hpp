// H3 (1.1.4): the host of the real Download Play (azl/dlp_real_net.hpp) ejects a guest that asks for the content of the child - an Azahar or a
// console that has no child of its own: AzaharLink never ships the Download Play child through the room, every player has their own copy. The
// game itself still counts that guest and says nothing; the plugin's thread names it on the host's screen, once per guest for the game's run.
// The name is the one the room gives the player (its nickname), NEVER a MAC (not on the screen, not in the log). Pure: the plugin keeps a
// ContentNotices and the room's last list of members (Plugin::NoteRealDlpHost); Tests/dlp_content_notice_test.cpp holds it.
#pragma once

#include <string>
#include <vector>

#include "azl/common.hpp"
#include "azl/room_members.hpp"

namespace azl {
namespace dlp_real {

/// The guests already named on the screen (the plugin's thread only).
class ContentNotices {
public:
    static constexpr u32 kGuests = 16;  // a host has at most 7 clients at a time; 16 different guests in one run is far more than a party
    /// True the first time `guest` is told of, and it is remembered; false for a guest already told of - and for every new one once kGuests
    /// are remembered (at most once per guest: never twice, rather not at all).
    bool First(const Mac& guest) {
        for (u32 i = 0; i < count_; ++i) {
            if (seen_[i] == guest) {
                return false;
            }
        }
        if (count_ >= kGuests) {
            return false;
        }
        seen_[count_++] = guest;
        return true;
    }
    u32 count() const {
        return count_;
    }

private:
    Mac seen_[kGuests] = {};
    u32 count_ = 0;
};

/// The name the room gives `guest` (the nickname of the member with that MAC in its last list), or "" if the room does not list it.
inline std::string GuestName(const std::vector<RoomMemberRow>& members, const Mac& guest) {
    for (const RoomMemberRow& m : members) {
        if (m.mac == guest) {
            return m.nickname;
        }
    }
    return std::string();
}

/// The text on the host's screen for a guest named `name` ("" = the room does not list it: "a player"). Never a MAC.
inline std::string ContentNoticeText(const std::string& name) {
    return "3DS Link: " + (name.empty() ? std::string("a player") : name) + " needs its own copy of the game (no download through the room)";
}

}  // namespace dlp_real
}  // namespace azl
