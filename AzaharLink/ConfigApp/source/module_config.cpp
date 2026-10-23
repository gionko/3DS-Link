// What the app tells the delivery module (titleshare_host/hostcfg.h): the module names no folder of its own, and its one log switch is the app's
// Debug log. Called when the data folder is opened, when the key file changes, when the log switch is flipped and before the module runs.
#include "ui.hpp"

extern "C" {
#include "../titleshare_host/hostcfg.h"
}

namespace ui {

void App::ApplyModuleConfig() {
    hc_set_app_dir(paths.appdir.c_str());
    hc_set_data_dir(paths.datadir.c_str());
    hc_set_boot9_path(boot9.found ? boot9.path.c_str() : "");
    hc_set_log_enabled(cfg.debug > 0 ? 1 : 0);
}

} // namespace ui
