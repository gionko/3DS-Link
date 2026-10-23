// The app puts the plugin it carries on the SD card by itself (plugin_install.hpp, plugin_resource.hpp): the App's side of it, which needs the paths, the settings, the config
// and the toast. The decisions and the file work are in plugin_install.*, tested on a PC.
#include "plugin_resource.hpp"
#include "ui.hpp"

namespace ui {

void App::LookAtPlugin(bool full) {
    paths.plugin_file = setup::ResolvePluginFile(paths); // (a plugin may have been put next to the app, or in its own folder, since)
    const std::string stamp = setup::FileStamp(paths.plugin_file);
    if (!full && plugin_card_known && plugin_card_path == paths.plugin_file && plugin_card_stamp == stamp) {
        return; // (the file is the one the last look saw: size and date: the megabyte is not read again at every return to the main page)
    }
    const setup::PluginProbe probe = setup::InspectPluginFile(paths.plugin_file, setup::EmbeddedPluginData(), setup::EmbeddedPluginSize());
    plugin_card = setup::ClassifyPluginCard(probe, setup::EmbeddedPluginVersionWord());
    plugin_ready = plugin_card == setup::PluginCard::Same;
    plugin_found_version = probe.header_ok ? setup::PluginVersionText(probe.version_word) : std::string();
    plugin_card_path = paths.plugin_file;
    plugin_card_stamp = stamp;
    plugin_card_known = true;
}

setup::PluginEnsure App::EnsurePlugin() {
    paths.plugin_file = setup::ResolvePluginFile(paths);
    setup::SetCurrentPaths(paths);
    const setup::PluginEnsure result = setup::EnsurePluginFile(paths.plugin_file, setup::EmbeddedPluginData(), setup::EmbeddedPluginSize(), setup::EmbeddedPluginUsable(),
                                                               setup::EmbeddedPluginVersionWord());
    // What the card holds now is what the ensure saw last (the megabyte is not read a second time), with the size and date of the file for the quiet looks that follow.
    plugin_card = result.after;
    plugin_ready = plugin_card == setup::PluginCard::Same;
    plugin_found_version = result.header_ok ? setup::PluginVersionText(result.version_word) : std::string();
    plugin_card_path = paths.plugin_file;
    plugin_card_stamp = setup::FileStamp(paths.plugin_file);
    plugin_card_known = true;
    if (result.step != setup::PluginStep::Leave) {
        plugin_tried = true;
        plugin_write = result.write;
    } else if (result.after == setup::PluginCard::Same) {
        plugin_tried = false; // (nothing was needed, or an earlier try has since been made unnecessary: the file is the plugin of the app)
        plugin_write = setup::WriteResult::Ok;
    }
    const std::string folder = setup::ParentDir(paths.plugin_file);
    const std::string say = setup::PluginInstallToast(result, folder);
    if (!say.empty()) {
        plugin_notice = say;
        plugin_notice_color = result.failed() ? col::kOff : col::kOn;
    }
    if (result.wrote()) {
        // The master file is another file now: the copies in the titles' folders are looked at again with it (the stamp that 3DSLink.cfg keeps is of the file that was there,
        // and an app that has already looked at them this time must look once more).
        plugin_copies_checked = false;
        plugin_stamp_pending.clear();
        app_cfg.plugin_stamp.clear();
        SaveAppConfig();
    }
    return result;
}

std::string App::PluginLockText() const {
    return setup::PluginLockText(setup::EmbeddedPluginUsable(), plugin_card, setup::ParentDir(paths.plugin_file), plugin_found_version, setup::kVersion, plugin_tried, plugin_write);
}

} // namespace ui
