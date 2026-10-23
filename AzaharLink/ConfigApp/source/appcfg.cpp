#include "appcfg.hpp"

#include "azl/ini_values.hpp"
#include "fileio.hpp"

namespace setup {

namespace {

constexpr std::size_t kMaxCfgBytes = 8 * 1024;

std::string Trim(const std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r')) {
        ++begin;
    }
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return s.substr(begin, end - begin);
}

} // namespace

AppConfig ParseAppConfig(const std::string& text) {
    AppConfig cfg;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string line = Trim(text.substr(pos, end - pos));
        pos = end + 1;
        const std::size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) {
            continue;
        }
        const std::string key = Trim(line.substr(0, eq));
        const std::string value = Trim(line.substr(eq + 1));
        if (key == "first_start_done") {
            cfg.first_start_done = value == "1";
        } else if (key == "boot9") {
            cfg.boot9_path = value;
        } else if (key == "plugin_stamp") {
            cfg.plugin_stamp = value;
        } else if (key == "dlp_delivery") {
            cfg.dlp_delivery = azl::ini::ParseBool(value, true); // the plugin's words (0/off/false/no shut it); a slip is the default, on
        }
    }
    return cfg;
}

std::string SerialiseAppConfig(const AppConfig& cfg) {
    std::string out = "# 3DS Link's own settings (the plugin's are in AzaharLink.ini)\n";
    out += std::string("first_start_done=") + (cfg.first_start_done ? "1" : "0") + "\n";
    if (!cfg.boot9_path.empty()) {
        out += "boot9=" + cfg.boot9_path + "\n";
    }
    if (!cfg.plugin_stamp.empty()) {
        out += "plugin_stamp=" + cfg.plugin_stamp + "\n";
    }
    out += std::string("dlp_delivery=") + (cfg.dlp_delivery ? "1" : "0") + "\n";
    return out;
}

bool LoadAppConfig(const std::string& path, AppConfig& cfg) {
    std::string content;
    if (ReadTextFile(path, kMaxCfgBytes, content) != LoadResult::Loaded) {
        return false;
    }
    cfg = ParseAppConfig(content);
    return true;
}

bool SaveAppConfig(const std::string& path, const AppConfig& cfg) {
    return WriteTextFile(path, SerialiseAppConfig(cfg));
}

} // namespace setup
