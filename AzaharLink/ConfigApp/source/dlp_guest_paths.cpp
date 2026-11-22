#include "dlp_guest_paths.hpp"

#include "azl/dlp_children.hpp"
#include "azl/title_rules.hpp"
#include "paths.hpp"

namespace dlpguest {

std::string GuestsDir() {
    return std::string(setup::kPluginDataDir) + "/" + kGuestsDirName;
}

std::string IndexPath() {
    return GuestsDir() + "/" + kIndexFileName;
}

std::string BlockPath(std::uint64_t prepared) {
    char name[azl::dlp_children::kBlockNameBytes];
    azl::dlp_children::BlockName(prepared, name);
    return GuestsDir() + "/" + name;
}

std::uint64_t PreparedFor(std::uint64_t game) {
    return azl::title_rules::PreparedOf(game);
}

} // namespace dlpguest
