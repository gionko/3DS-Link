// The lines of the latency of the other players for the chat's screen (azl/chat_host.hpp LatencyBoard, azl/chat_draw.hpp DrawLatencyLines):
// built on the plugin's thread from the estimates of azl/latency.hpp and the names the game gave the nodes. Pure: the tests build it on the PC.
//
// One line a player that still sends packets, in the order of the nodes, while there are at most kLatShown others (up to four players in the lobby);
// with more, ONE line of tiles with every player in it (all of them are in latency.log, with their names and values, whatever the screen shows). nick = the name (the UDS username: 10 UTF-16 units, kept when they are plain ASCII, else "P<node>";
// the host, node 1, is marked "[H] " before its name, which is cut as everywhere else: "[H] Carlo Ma.:28ms"), text = "64ms" ("~64ms" for an old value, "--" for none yet), pad = the colour's index (LatencyColour).
#pragma once

#include <cstring>

#include "azl/chat_core.hpp"
#include "azl/chat_draw.hpp"
#include "azl/latency.hpp"

namespace azl {
namespace chat {

constexpr u32 kLatName = 10; ///< UTF-16 units in a UDS username
/// Players listed with a name at most: the stack of the latency (3 blocks of 16 rows from the screen's edge: 48 rows) is where the chat, even five messages
/// of three lines (180 rows from the other edge), cannot reach, so nothing of the chat ever covers it and it never has to hold the chat back.
/// Four players (3 others) are the games tried (Monster Hunter, Final Fantasy Explorers, Mario Party); more than that (a Tetris-like game takes 8 in the lobby)
/// are one strip of small coloured tiles, one a player, that fits in one row (BuildLatencyRows, azl/chat_draw.hpp kLatTiles).
#ifndef AZL_LAT_SHOWN
#define AZL_LAT_SHOWN 3 // (the screenshots' tool tries 7 names: the whole space between the latency and the chat)
#endif
constexpr u32 kLatShown = AZL_LAT_SHOWN;
static_assert(kLatShown != 3 || kTopY + static_cast<int>(kLatShown) * (BlockHeight(1) + kGapY) + static_cast<int>(kLines) * (BlockHeight(kBlockLines) + kGapY) + (240 - kBottomY) <= 240,
              "the latency's stack and the chat's five longest messages fit the top screen together");

/// The name of `node` from its UDS username (`units`, kLatName of them, 0-terminated or not): printable ASCII as it is, trailing spaces cut;
/// "P<node>" when nothing is left or no unit is plain ASCII (a name in another script would be a row of question marks). Returns its length.
inline u32 LatencyName(int node, const u16* units, char* out) {
    u32 n = 0;
    u32 plain = 0;
    if (units != nullptr) {
        for (u32 i = 0; i < kLatName && units[i] != 0; ++i) {
            const u16 c = units[i];
            const bool ok = c >= 0x20 && c <= 0x7E;
            plain += ok ? 1 : 0;
            out[n++] = ok ? static_cast<char>(c) : '?';
        }
        while (n > 0 && out[n - 1] == ' ') {
            --n;
        }
    }
    if (n == 0 || plain == 0) {
        n = 0;
        out[n++] = 'P';
        if (node >= 10) {
            out[n++] = static_cast<char>('0' + node / 10);
        }
        out[n++] = static_cast<char>('0' + node % 10);
    }
    out[n] = '\0';
    return n;
}

/// "64ms" / "~64ms" / "--" into `out` (at most 12 characters with the terminator); "64ms!" while the value is the last 2 s, clearly worse than the window
/// (latency::NodeRtt::spike: the connection just got worse - the user's request, 2026-11-13). Returns the length.
inline u32 LatencyText(int rtt_ms, bool stale, char* out, bool spike = false) {
    u32 n = 0;
    if (rtt_ms < 0) {
        out[n++] = '-';
        out[n++] = '-';
        out[n] = '\0';
        return n;
    }
    if (stale) {
        out[n++] = '~';
    }
    if (rtt_ms > 9999) {
        rtt_ms = 9999;
    }
    char digits[5];
    u32 d = 0;
    int v = rtt_ms;
    do {
        digits[d++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v > 0);
    while (d > 0) {
        out[n++] = digits[--d];
    }
    out[n++] = 'm';
    out[n++] = 's';
    if (spike) {
        out[n++] = '!';
    }
    out[n] = '\0';
    return n;
}

/// The rows of the board: `nr` as latency::Snapshot gives it (8 nodes, node 1 first), `names[node]` the name to show for each node (plain ASCII, the
/// room's nickname of that player - the one the chat writes - else its UDS username; `has_name[node]` false: none known: "P<node>"), `now_ms` the
/// time stamped on the rows. Fills `rows` (kLatShown at most: one line a player up to
/// kLatShown players, one line of tiles above that), returns how many.
inline u32 BuildLatencyRows(const latency::NodeRtt nr[8], const char names[9][kNickMax + 1], const bool has_name[9], u32 now_ms, Line* rows) {
    u32 present = 0;
    for (int i = 0; i < 8; ++i) {
        present += nr[i].age_ms >= 0 && nr[i].node >= 1 && nr[i].node <= 8 ? 1 : 0;
    }
    if (present > kLatShown) { // more than four players in the lobby: one strip of tiles, "<node digit or H for the host><latency colour>" a player (see kLatTiles)
        Line& l = rows[0];
        l = Line();
        l.t_ms = now_ms;
        u32 n = 0;
        for (int i = 0; i < 8; ++i) {
            const latency::NodeRtt& o = nr[i];
            if (o.age_ms < 0 || o.node < 1 || o.node > 8) {
                continue;
            }
            l.text[n++] = o.node == 1 ? 'H' : static_cast<char>('0' + o.node); // (the host of the match, node 1, is marked H)
            l.text[n++] = static_cast<char>('0' + LatencyColour(o.rtt_ms));
        }
        l.text[n] = '\0';
        l.text_len = static_cast<u8>(n);
        l.pad = kLatTiles;
        return 1;
    }
    u32 count = 0;
    for (int i = 0; i < 8 && count < kLatShown; ++i) {
        const latency::NodeRtt& o = nr[i];
        if (o.age_ms < 0 || o.node < 1 || o.node > 8) {
            continue; // (no packet from this node lately: not in the match, or gone)
        }
        Line& l = rows[count++];
        l = Line();
        l.t_ms = now_ms;
        l.sep = '\0'; // ("Ana:28ms": no space after the colon, one character more for the name)
        // The name as the chat shows a nick (CutNick): 8 characters at most, then a "." when there were more - "Carlo Ma."; the log has the whole name.
        u32 n = 0;
        if (has_name[o.node]) {
            n = Sanitize(reinterpret_cast<const u8*>(names[o.node]), std::strlen(names[o.node]), l.nick, kNickMax, false);
        }
        if (n == 0) {
            n = LatencyName(o.node, nullptr, l.nick);
        }
        // The host is marked "[H] " before the name (not part of the nickname). Its name is cut like every other - the same 8 characters the chat shows
        // for his messages - so the name read in the chat is the name read here ("[H] Carlo Ma.:28ms"; it was cut to 5 before, and no longer matched).
        n = CutNick(l.nick, n);
        if (o.node == 1) {
            std::memmove(l.nick + 4, l.nick, n + 1);
            l.nick[0] = '['; l.nick[1] = 'H'; l.nick[2] = ']'; l.nick[3] = ' ';
            n += 4;
        }
        l.own = static_cast<u8>(o.node - 1); // (the colour of the node: kNodeColour, the same the player's messages have in the chat)
        l.nick_len = static_cast<u8>(n);
        l.text_len = static_cast<u8>(LatencyText(o.rtt_ms, o.stale, l.text, o.spike));
        l.pad = LatencyColour(o.rtt_ms);
    }
    return count;
}

} // namespace chat
} // namespace azl
