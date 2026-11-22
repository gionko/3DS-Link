// The modes of the Download Play column of azl/title_rules.hpp (1.1.4, L4): one enum and nothing else, so that the table of the titles -
// which every unit of the engine reads through azl/flow_relief.hpp - names the modes without including the lobby's types (azl/dlp_types.hpp
// includes this header: the lobby, the hub and the plugin see the same enum).
// Pure header: no state, no allocation, no log.
#pragma once

#include "azl/common.hpp"

namespace azl {

/// What the plugin does with the Download Play services of a game (the Download Play column of azl/title_rules.hpp): nothing, answer the
/// wireless check only, carry the lobby in the plugin's dialect, or carry it speaking the REAL Download Play on the wire. New modes go at the END.
/// SystemClient (1.1.4, G1) is never a game's: it is the system's Download Play application (ctr/text_search_guard.hpp's six ids, the plugin's
/// DlpModeOf), whose dlp:CLNT the plugin serves as a client of the real Download Play over the room; no row of the table says it.
enum class DlpGameMode : u8 { None = 0, ProbeOnly = 1, Lobby = 2, RealDlp = 3, SystemClient = 4 };

} // namespace azl
