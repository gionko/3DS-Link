// Where the app writes what the plugin of the system's Download Play application reads of the Download Play children 3DS Link prepares
// (1.1.4, contract K2 of analysis/piano-1.1.4-2026-11-15/specifiche/7-CONTRATTO.md): the index and the blocks of azl/dlp_children.hpp, in a
// folder of their own under the plugin's fixed data folder (setup::kPluginDataDir, NOT the app's own folder: the plugin finds them without the
// app). Not dlp/, which is 3DS Link Delivery's (paths.hpp kDlpDirName). Plain C++, so Tests/dlp_children_test.cpp builds this very file and
// holds these paths to the plugin's (azl::dlp_children::kIndexPath, kGuestsDir).
#pragma once

#include <cstdint>
#include <string>

namespace dlpguest {

constexpr const char* kGuestsDirName = "dlp-guests";  // the folder, in setup::kPluginDataDir
constexpr const char* kIndexFileName = "index.txt";   // the index, in that folder

std::string GuestsDir();                       // sdmc:/3ds/3DSLink/dlp-guests
std::string IndexPath();                       // sdmc:/3ds/3DSLink/dlp-guests/index.txt
std::string BlockPath(std::uint64_t prepared); // sdmc:/3ds/3DSLink/dlp-guests/<prepared, 16 digits>.bin
/// The prepared child of the application `game` (azl::title_rules::PreparedOf), 0 = none is possible.
std::uint64_t PreparedFor(std::uint64_t game);

} // namespace dlpguest
