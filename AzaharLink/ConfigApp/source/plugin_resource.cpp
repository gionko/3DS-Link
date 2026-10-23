#include "plugin_resource.hpp"

#include <cstring>
#include <string>

#include "AzaharLink_3gx.h" // the resource that data/AzaharLink.3gx is linked in as (devkitPro's bin2o)
#include "settings.hpp"

namespace setup {

const std::uint8_t* EmbeddedPluginData() {
    return AzaharLink_3gx;
}

std::size_t EmbeddedPluginSize() {
    return AzaharLink_3gx_size;
}

std::uint32_t EmbeddedPluginVersionWord() {
    if (AzaharLink_3gx_size < 12 || std::memcmp(AzaharLink_3gx, "3GX$", 4) != 0) {
        return 0;
    }
    std::uint32_t word = 0;
    std::memcpy(&word, AzaharLink_3gx + 8, 4);
    return word;
}

bool EmbeddedPluginUsable() {
    const std::uint32_t word = EmbeddedPluginVersionWord();
    return word != 0 && PluginVersionText(word) == kVersion;
}

} // namespace setup
