// A console that hosts one of the titles of azl/title_rules.hpp with host_keeps_to_its_game answers no newcomer that, in the room, plays another game (1.1.4, 2026-11-14).
//
// Mario Party: Island Tour hosted by a console (the European application, 2026-11-14 evening): a member of the room that had Tetris
// open came into its network as node 2 and held a place of the party. Its MAC is of an Azahar; how an emulator in another game came to
// list and join this network has not been read in Azahar's source (the guess: its scan lists the networks of every game). The game's
// own guests come from the Download Play application of the system (or from the child it starts), and those never say "Mario Party"
// to the room: the system application and the child have ids of their own, or no id at all.
//
// So the rule is narrow, and every side of it lets in what it does not know:
//   - it applies only while the console hosts one of those titles as the APPLICATION (title id high word 00040000);
//   - the newcomer is refused only when the room lists, for its MAC, a game that was SOLD (an application whose unique id is in the
//     ranges Nintendo gave out - the same two ranges the app reads in ConfigApp/source/games.hpp) and is not this game in any region
//     (the same row of azl/title_rules.hpp);
//   - a member that says no game (id 0), the Download Play application (00040010), a Download Play child (00040001), a homebrew outside
//     the ranges, a member the room does not list: all of them are answered as before.
// A refused newcomer gets no answer, as when the network is full (Azahar sends no reject frame): its join fails and it stays in the room.
// A node already in the network is never put out by this rule.
//
// The rows hold each game in ALL its regions (the catalogue of 3DS Link). Pure header: no engine, no room, no allocation.
#pragma once

#include "azl/common.hpp"
#include "azl/title_rules.hpp"

namespace azl {
namespace one_game {

inline constexpr u32 kTitleHighApplication = title_rules::kTitleHighApplication;

/// The unique ids (title id >> 8, 24 bits) of the games that were sold, as ConfigApp/source/games.hpp has them (kSoldIdFirst ...).
inline constexpr u32 kSoldIdFirst = 0x00300;
inline constexpr u32 kSoldIdLast = 0x01FFF;
inline constexpr u32 kSoldNewIdFirst = 0xF7000;
inline constexpr u32 kSoldNewIdLast = 0xF71FF;

/// Does a console that runs `own_title` keep its network to its game? Only as the application of a title whose row says so.
constexpr bool HostKeepsToItsGame(u64 own_title) {
    return static_cast<u32>(own_title >> 32) == kTitleHighApplication && title_rules::RulesFor(own_title).host_keeps_to_its_game;
}

/// Is `title` a sold game (an application whose unique id is in one of the two ranges)?
constexpr bool IsSoldGame(u64 title) {
    if (static_cast<u32>(title >> 32) != kTitleHighApplication) {
        return false;
    }
    const u32 unique = static_cast<u32>(title >> 8) & 0xFFFFFFu;
    return (unique >= kSoldIdFirst && unique <= kSoldIdLast) || (unique >= kSoldNewIdFirst && unique <= kSoldNewIdLast);
}

/// Does a member of the room that plays `member_title` (the id the room lists for it; 0 = none) play another game than a host of
/// `own_title`? Only for a host that keeps to its game, only for a sold game, and never for the same game in another region.
constexpr bool PlaysAnotherGame(u64 own_title, u64 member_title) {
    return HostKeepsToItsGame(own_title) && IsSoldGame(member_title) && !title_rules::SameGame(own_title, member_title);
}

} // namespace one_game
} // namespace azl
