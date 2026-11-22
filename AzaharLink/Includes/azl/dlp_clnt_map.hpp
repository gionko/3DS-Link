// The numbers of dlp:CLNT, the Download Play client of the SYSTEM's Download Play application, and the small pure decisions on them (1.1.4, G1 of
// analysis/piano-1.1.4-2026-11-15/specifiche/1-G1.md section 1.1). dlp:CLNT is dlp:FKCL with eight more commands: most of its ids have the
// semantics of an FKCL id (ToFkcl), the Download Play application (EUR v3073, read from its code: analysis/island-tour-2026-11-13/plugin/
// modalita-download/RAPPORTO.md section 1) calls fifteen of the twenty (UsedByDlplay), and the room's Download Play is used only by a plugin
// that joins a room by itself (UsesRoom). The small line under a host's name is NOT here: it is dlp_children::MakeLabel / WriteLabel
// (azl/dlp_children.hpp), the one writer of it (contract C7).
// Pure header: constants and constant expressions, no state, no allocation, no log. All of it is called on the Download Play application's thread.
#pragma once

#include "azl/common.hpp"
#include "azl/dlp_types.hpp"  // (the client states: kCliStarted)

#ifndef AZL_CLNT_SYSTEM_JOINS
#define AZL_CLNT_SYSTEM_JOINS 0
#endif

namespace azl {
namespace clnt {

/// 1.1.4 (K10, route B): the system's Download Play application never joins: the join is the 3DS Link app's (spec 8). Only the tests of G1's join
/// path set it (-DAZL_CLNT_SYSTEM_JOINS=1 on their lines of Tests/run.sh and Tests/ipc_test/build.sh); with it false the compiler drops that path.
inline constexpr bool kSystemAppJoins = AZL_CLNT_SYSTEM_JOINS != 0;

// >>> game-thread code (Tests/policy_check.sh: the dispatch of dlp:CLNT runs on the Download Play application's thread)

// Ids of dlp:CLNT (Azahar dlp_clnt.cpp; the Download Play application, RAPPORTO section 1).
inline constexpr u32 kInitialize = 0x01, kFinalize = 0x02, kGetEventDescription = 0x03, kGetChannels = 0x04, kStartScan = 0x05, kStopScan = 0x06,
                     kGetServerInfo = 0x07, kGetTitleInfo = 0x08, kGetTitleInfoInOrder = 0x09, kDeleteScanInfo = 0x0A,
                     kPrepareForSystemDownload = 0x0B, kStartSystemDownload = 0x0C, kStartSession = 0x0D, kGetMyStatus = 0x0E,
                     kGetConnectingNodes = 0x0F, kGetNodeInfo = 0x10, kGetWirelessRebootPassphrase = 0x11, kStopSession = 0x12,
                     kGetCupVersion = 0x13, kGetDupAvailability = 0x14, kLast = 0x14;
/// ToFkcl of an id with no dlp:FKCL twin.
inline constexpr u32 kNoFkcl = 0xFF;
/// The state GetMyStatus shows instead of kCliStarted: "waiting, nothing to download" (the application moves on with 6, 7 and 9 alike, and 7 is
/// what a client that already has the child says).
inline constexpr u8 kCliShownStarted = 7;

constexpr u32 Bit(u32 id) {
    return 1u << id;
}
/// The ids the Download Play application calls: 01 02 04 05 06 07 09 0B 0C 0D 0E 11 12 13 14 (fifteen; never 03, 08, 0A, 0F, 10).
inline constexpr u32 kUsedByDlplay = Bit(kInitialize) | Bit(kFinalize) | Bit(kGetChannels) | Bit(kStartScan) | Bit(kStopScan) | Bit(kGetServerInfo) |
                                     Bit(kGetTitleInfoInOrder) | Bit(kPrepareForSystemDownload) | Bit(kStartSystemDownload) | Bit(kStartSession) |
                                     Bit(kGetMyStatus) | Bit(kGetWirelessRebootPassphrase) | Bit(kStopSession) | Bit(kGetCupVersion) | Bit(kGetDupAvailability);
/// The ids posted to the network thread (they change the lobby or the radio): 01 02 05 06 0D 12. Every other id is served on the caller's thread.
inline constexpr u32 kPosted = Bit(kInitialize) | Bit(kFinalize) | Bit(kStartScan) | Bit(kStopScan) | Bit(kStartSession) | Bit(kStopSession);
/// The ids whose request carries a host's MAC in words 1 and 2 (the trace zeroes them): 07 08 0A 0B 0D 13 14.
inline constexpr u32 kCarriesMac = Bit(kGetServerInfo) | Bit(kGetTitleInfo) | Bit(kDeleteScanInfo) | Bit(kPrepareForSystemDownload) | Bit(kStartSession) |
                                   Bit(kGetCupVersion) | Bit(kGetDupAvailability);

constexpr bool In(u32 id, u32 set) {
    return id <= kLast && ((set >> id) & 1u) != 0;
}

/// The dlp:FKCL id whose semantics the dlp:CLNT id has: 01-0A the same number, 0D-12 two less (FKCL 0B-10); kNoFkcl for 0B, 0C, 13, 14, 0
/// and the ids past kLast.
constexpr u32 ToFkcl(u32 id) {
    return (id >= kInitialize && id <= kDeleteScanInfo) ? id : ((id >= kStartSession && id <= kStopSession) ? id - 2 : kNoFkcl);
}
constexpr bool UsedByDlplay(u32 id) {
    return In(id, kUsedByDlplay);
}
/// Served on the calling thread (tables, constants, the book): 03 04 07 08 09 0A 0B 0C 0E 0F 10 11 13 14, id 0 and the ids past kLast (answered
/// with an error there). Posted: kPosted.
constexpr bool IsInline(u32 id) {
    return !In(id, kPosted);
}
constexpr bool CarriesMac(u32 id) {
    return In(id, kCarriesMac);
}

/// Does the Download Play application use the room's Download Play? Only when the plugin joins a room BY ITSELF: the same formula as
/// Sources/main.cpp (g_want_connected = (cfg.autoconnect || g_menu_off) && !cfg.host.empty()). Decided once, from the settings, in EarlyInit.
constexpr bool UsesRoom(bool host_set, bool autoconnect, bool menu_off) {
    return host_set && (autoconnect || menu_off);
}

/// GetMyStatus as the Download Play application reads it: the lobby's word (state << 24 | in-session << 16 | node) with the state kCliStarted
/// shown as kCliShownStarted; the low 24 bits as they are.
constexpr u32 ShownWord(u32 lobby_word) {
    return (lobby_word >> 24) == kCliStarted ? ((static_cast<u32>(kCliShownStarted) << 24) | (lobby_word & 0x00FFFFFFu)) : lobby_word;
}

// <<< game-thread code

static_assert(ToFkcl(kStartSession) == 0x0B && ToFkcl(kStopSession) == 0x10 && ToFkcl(kGetWirelessRebootPassphrase) == 0x0F,
              "dlp:CLNT 0D..12 are dlp:FKCL 0B..10");
static_assert(kCliStarted == 6 && kCliShownStarted == 7, "the distribution's state is shown as 7");

} // namespace clnt
} // namespace azl
