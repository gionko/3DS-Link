// The plugin that is INSIDE the app: data/AzaharLink.3gx, linked in by the Makefile (devkitPro's bin2o: AzaharLink_3gx, AzaharLink_3gx_size). DlpLink/tools/build_all.sh builds the
// plugin first and copies it there, so after a build the resource is the plugin of THAT build (check_chain.py proves that the app's ELF, .3dsx and CIA carry exactly it, by size and
// SHA-256, and that its header says the version in AzaharLink/VERSION). A tree that was never built holds the plugin of its last build there, so that the build always has one.
//
// The app puts this plugin on the SD card (plugin_install.hpp), and the Download Play child is composed from it (dlp_prepare.cpp): the card is never the source of either.
#pragma once

#include <cstddef>
#include <cstdint>

namespace setup {

const std::uint8_t* EmbeddedPluginData();
std::size_t EmbeddedPluginSize();
// The version word of its header (bytes 8..11: major << 24 | minor << 16 | revision << 8); 0 when the resource does not start like a .3gx.
std::uint32_t EmbeddedPluginVersionWord();
// A plugin that can be put on the card and handed to the composer: it starts like a .3gx and says the version of this app (kVersion). A build whose resource is not one
// (a placeholder) says so everywhere, and puts nothing on the card.
bool EmbeddedPluginUsable();

} // namespace setup
