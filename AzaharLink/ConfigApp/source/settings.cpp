#include "settings.hpp"

#include <sys/stat.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace setup {

static_assert(azl::ini::kPortDefault == azl::room::kDefaultPort, "the ini default port is the room default port");

namespace {

// The plugin refuses an ini larger than 4096 bytes, so never write more than this.
constexpr std::size_t kMaxIniBytes = 3800;
constexpr std::size_t kMaxReadBytes = 16 * 1024;
constexpr std::size_t kMaxUnknownLines = 24;
constexpr std::size_t kMaxUnknownLineLen = 200;

// Makes every folder of `dir` ("sdmc:/a/b": sdmc:/a, then sdmc:/a/b); a folder that is there is no error.
void MakeDirChain(const std::string& dir) {
    for (std::size_t at = dir.find('/', dir.find(':') + 2); at != std::string::npos; at = dir.find('/', at + 1)) {
        mkdir(dir.substr(0, at).c_str(), 0777);
    }
    mkdir(dir.c_str(), 0777);
}

std::string TrimmedLine(const std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (end > begin && (s[end - 1] == '\r' || s[end - 1] == '\n' || s[end - 1] == ' ' || s[end - 1] == '\t')) {
        --end;
    }
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) {
        ++begin;
    }
    if (end - begin >= 3 && s.compare(begin, 3, "\xEF\xBB\xBF") == 0) {
        // (a file saved with a BOM by a Windows editor: it belongs to no line, a comment must not keep it)
        for (begin += 3; begin < end && (s[begin] == ' ' || s[begin] == '\t'); ++begin) {
        }
    }
    return s.substr(begin, end - begin);
}

} // namespace

bool ValidNickname(const std::string& s) {
    if (s.size() < 4 || s.size() > 20) {
        return false;
    }
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == ' ' || c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool MenuOn(const Settings& cfg) {
    return !azl::ini::MenuKeyIsOff(cfg.menu_key);
}

std::string MenuChordText(const std::string& chord) {
    // Same separators as the plugin's chord parser (Sources/main.cpp): a token "select" in any case is Select.
    bool has_select = false;
    std::string token;
    for (std::size_t i = 0; i <= chord.size(); ++i) {
        const char c = i < chord.size() ? chord[i] : '+';
        if (c == '+' || c == ' ' || c == ',') {
            std::string lower;
            for (char t : token) {
                lower += (t >= 'A' && t <= 'Z') ? static_cast<char>(t - 'A' + 'a') : t;
            }
            has_select = has_select || lower == "select";
            token.clear();
        } else {
            token += c;
        }
    }
    return has_select ? chord : chord + "+Select";
}

void SetMenuOn(Settings& cfg, bool on) {
    if (on == MenuOn(cfg)) {
        return;
    }
    if (on) {
        cfg.menu_key = cfg.menu_chord.empty() ? azl::ini::kDefaultMenuKey : cfg.menu_chord;
    } else {
        if (!cfg.menu_key.empty()) {
            cfg.menu_chord = cfg.menu_key;
        }
        cfg.menu_key = azl::ini::kMenuKeyOff;
    }
}

const char* SleepStopText(int value) {
    return value == 1 ? "Client only" : value == 2 ? "Always" : "Off";
}

int NextSleepStop(int value) {
    return value == 0 ? 1 : value == 1 ? 2 : 0;
}

const char* RadioLikeText(int value) {
    return value == 1 ? "Broadcast" : value == 2 ? "All (twice)" : "Off";
}

int NextRadioLike(int value) {
    return value == 0 ? 1 : value == 1 ? 2 : 0;
}

int PrevRadioLike(int value) {
    return value == 0 ? 2 : value == 1 ? 0 : 1;
}

int PrevSleepStop(int value) {
    return value == 0 ? 2 : value == 1 ? 0 : 1;
}

std::string PluginVersionText(u32 word) {
    return std::to_string(word >> 24) + "." + std::to_string((word >> 16) & 0xFF) + "." + std::to_string((word >> 8) & 0xFF);
}

LoadResult LoadSettings(Settings& cfg) {
    std::string content;
    cfg.moved_from_old_place = false;
    LoadResult read = ReadTextFile(kIniPath, kMaxReadBytes, content);
    if (read == LoadResult::Missing) {
        // The old place (sdmc:/luma/plugins/AzaharLink.ini, where the app put it until 2026-10-19): the file is moved to the new one, whole,
        // so that a plugin older than this app (it reads the old place when the new file is missing) and this app agree on one file. An
        // old file that cannot be read is left alone, like any file that cannot be read.
        const LoadResult old = ReadTextFile(kOldIniPath, kMaxReadBytes, content);
        if (old == LoadResult::Loaded) {
            MakeDirChain(kPluginDataDir);
            if (WriteTextFile(kIniPath, content)) {
                std::remove(kOldIniPath);
                cfg.moved_from_old_place = true;
            }
            read = LoadResult::Loaded;
        } else if (old == LoadResult::Unreadable) {
            return old;
        }
    }
    if (read != LoadResult::Loaded) {
        return read;
    }

    cfg.unknown_lines.clear();
    cfg.keys_seen = 0;
    std::size_t pos = 0;
    while (pos < content.size()) {
        std::size_t end = content.find('\n', pos);
        if (end == std::string::npos) {
            end = content.size();
        }
        const std::string raw = content.substr(pos, end - pos);
        pos = end + 1;
        const azl::ini::KeyValue kv = azl::ini::SplitLine(raw);
        const std::string l = TrimmedLine(raw);
        if (l.empty()) {
            continue;
        }
        const std::string& key = kv.key;
        const std::string& value = kv.value;
        int known = -1; // the index of the key in azl::ini::kKeyNames, or -1
        for (int i = 0; kv.ok && i < azl::ini::kKeyCount; ++i) {
            if (key == azl::ini::kKeyNames[i]) {
                known = i;
            }
        }
        if (known >= 0) {
            cfg.keys_seen |= 1u << known;
        }
        if (key == "enabled") {
            cfg.enabled = azl::ini::ParseBool(value, true);
        } else if (key == "host") {
            cfg.host = value;
        } else if (key == "port") {
            const long p = std::strtol(value.c_str(), nullptr, 10);
            if (p > 0 && p < 65536) {
                cfg.port = static_cast<u16>(p);
            }
        } else if (key == "nickname") {
            // Older versions saved a placeholder name; it counts as "not chosen".
            cfg.nickname = value == "3DS Player" ? std::string() : value;
        } else if (key == "password") {
            cfg.password = value;
        } else if (key == "tunnel") {
            cfg.tunnel = azl::ini::ParseBool(value, true);
        } else if (key == "autoconnect") {
            cfg.autoconnect = azl::ini::ParseBool(value, true);
        } else if (key == "radio_like") {
            cfg.radio_like = azl::ini::ParseRadioLike(value);
        } else if (key == "socket_buffers") {
            cfg.socket_buffers = azl::ini::ParseBool(value, true);
        } else if (key == "net_thread_core") {
            const long c = std::strtol(value.c_str(), nullptr, 10);
            if (c >= -1 && c <= 3) { // (the plugin's range: 2 and 3 are the extra cores of a New 3DS, the A/B of the lag session)
                cfg.net_core = static_cast<int>(c);
            }
        } else if (key == "menu_key") {
            if (!value.empty()) {
                cfg.menu_key = value; // (a chord, or off/none: carried as it is)
                if (!azl::ini::MenuKeyIsOff(value)) {
                    cfg.menu_chord = value;
                }
            }
        } else if (key == "game_name") {
            cfg.game_name = value;
        } else if (key == "debug") {
            cfg.debug = azl::ini::DebugLevel(value);
        } else if (key == "rcvbuf_kb") {
            cfg.rcvbuf_kb = azl::ini::RcvbufKb(value);
        } else if (key == "sleep_stop") {
            cfg.sleep_stop = azl::ini::SleepStop(value);
        } else if (cfg.unknown_lines.size() < kMaxUnknownLines && l.size() <= kMaxUnknownLineLen) {
            cfg.unknown_lines.push_back(l);
        }
    }
    return LoadResult::Loaded;
}

bool PluginsDirExists() {
    struct stat st;
    return stat(kPluginsDir, &st) == 0 && S_ISDIR(st.st_mode);
}

bool HasAllKeys(const Settings& cfg) {
    return cfg.keys_seen == (1u << azl::ini::kKeyCount) - 1;
}

bool SaveSettings(const Settings& cfg) {
    MakeDirChain(kPluginDataDir);

    // The same writer as the plugin's: all 15 keys, always, in one order (azl/ini_values.hpp).
    azl::ini::Fields fields;
    fields.enabled = cfg.enabled;
    fields.host = cfg.host;
    fields.port = cfg.port;
    fields.nickname = cfg.nickname;
    fields.password = cfg.password;
    fields.tunnel = cfg.tunnel;
    fields.autoconnect = cfg.autoconnect;
    fields.radio_like = cfg.radio_like;
    fields.socket_buffers = cfg.socket_buffers;
    fields.net_thread_core = cfg.net_core;
    fields.menu_key = cfg.menu_key;
    fields.game_name = cfg.game_name;
    fields.debug = cfg.debug;
    fields.rcvbuf_kb = cfg.rcvbuf_kb;
    fields.sleep_stop = cfg.sleep_stop;
    std::vector<std::string> extra;
    for (const std::string& line : cfg.unknown_lines) {
        if (line.size() <= kMaxUnknownLineLen) {
            extra.push_back(line);
        }
    }
    return WriteTextFile(kIniPath, azl::ini::Render(fields, extra, kMaxIniBytes));
}

// ---------------------------------------------------------------------------------------------
// The saved rooms
// ---------------------------------------------------------------------------------------------

namespace {

constexpr std::size_t kMaxHostChars = 80; // what the keyboard accepts for an address

std::string LowerAscii(std::string s) {
    for (char& c : s) {
        c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return s;
}

} // namespace

bool ValidServerName(const std::string& name) {
    if (name.size() > kMaxServerNameChars) {
        return false;
    }
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == ' ' || c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool Server::Same(const Server& other) const {
    return port == other.port && LowerAscii(host) == LowerAscii(other.host);
}

std::string Server::Text() const {
    return host + ":" + std::to_string(port);
}

std::string Server::Label() const {
    return name.empty() ? Text() : name;
}

ServerParse ParseServer(const std::string& text, Server& out) {
    const std::string input = TrimmedLine(text);
    std::string host = input;
    long port = azl::room::kDefaultPort;
    const std::size_t colon = input.rfind(':');
    if (colon != std::string::npos) {
        host = TrimmedLine(input.substr(0, colon));
        const std::string port_text = TrimmedLine(input.substr(colon + 1));
        char* end = nullptr;
        port = std::strtol(port_text.c_str(), &end, 10);
        if (port_text.empty() || *end != '\0') {
            port = -1;
        }
    }
    bool host_ok = !host.empty() && host.size() <= kMaxHostChars;
    for (char c : host) {
        host_ok = host_ok && static_cast<unsigned char>(c) > ' ' && c != 0x7F && c != '|'; // no blank, no control character, no bar (it ends the host in the file)
    }
    if (!host_ok) {
        return ServerParse::BadHost;
    }
    if (port <= 0 || port >= 65536) {
        return ServerParse::BadPort;
    }
    out.host = host;
    out.port = static_cast<u16>(port);
    return ServerParse::Ok;
}

ServerParse ParseServerLine(const std::string& line, Server& out) {
    const std::size_t bar = line.find('|');
    const ServerParse parsed = ParseServer(bar == std::string::npos ? line : line.substr(0, bar), out);
    if (parsed != ServerParse::Ok) {
        return parsed;
    }
    out.name.clear();
    out.password.clear();
    if (bar != std::string::npos) {
        const std::size_t bar2 = line.find('|', bar + 1);
        const std::string name = TrimmedLine(bar2 == std::string::npos ? line.substr(bar + 1) : line.substr(bar + 1, bar2 - bar - 1));
        if (ValidServerName(name)) {
            out.name = name; // (an invalid name is dropped; the room stays)
        }
        if (bar2 != std::string::npos) {
            out.password = line.substr(bar2 + 1); // (as typed: only the end of the line is cut off)
            while (!out.password.empty() && (out.password.back() == '\r' || out.password.back() == '\n')) {
                out.password.pop_back();
            }
        }
    }
    return ServerParse::Ok;
}

ServerList ServerList::Parse(const std::string& text) {
    ServerList list;
    std::size_t pos = 0;
    while (pos < text.size() && !list.full()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        Server server;
        if (ParseServerLine(text.substr(pos, end - pos), server) == ServerParse::Ok && list.Find(server) < 0) {
            list.entries_.push_back(server);
        }
        pos = end + 1;
    }
    return list;
}

std::string ServerList::Serialise() const {
    std::string out;
    for (const Server& s : entries_) {
        out += s.host;
        if (s.port != azl::room::kDefaultPort) {
            out += ":" + std::to_string(s.port);
        }
        if (!s.name.empty() || !s.password.empty()) {
            out += "|" + s.name;
        }
        if (!s.password.empty()) {
            out += "|" + s.password;
        }
        out += "\n";
    }
    return out;
}

int ServerList::Find(const Server& server) const {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].Same(server)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

ServerList::AddResult ServerList::Add(const Server& server) {
    Server entry = server; // (a copy: the caller may hand over one of the list's own entries)
    const int at = Find(entry);
    if (at < 0 && full()) {
        return AddResult::Full;
    }
    if (at >= 0) {
        const Server& stored = entries_[static_cast<std::size_t>(at)];
        if (entry.name.empty()) {
            entry.name = stored.name; // (the room that is used again keeps the name it had)
        }
        if (entry.password.empty()) {
            entry.password = stored.password; // (... and its password: a caller that knows only the address must not wipe it)
        }
        entries_.erase(entries_.begin() + at);
    }
    entries_.insert(entries_.begin(), entry);
    return at >= 0 ? AddResult::Moved : AddResult::Added;
}

bool ServerList::Use(const Server& server) {
    const Server entry = server;
    if (Find(entry) == 0) {
        return false;
    }
    if (Find(entry) < 0 && full()) {
        entries_.pop_back();
    }
    Add(entry);
    return true;
}

ServerList::ReplaceResult ServerList::Replace(std::size_t index, const Server& server) {
    if (index >= entries_.size()) {
        return ReplaceResult::Missing;
    }
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (i != index && entries_[i].Same(server)) {
            return ReplaceResult::Duplicate;
        }
    }
    entries_[index] = server;
    return ReplaceResult::Ok;
}

bool ServerList::Remove(std::size_t index) {
    if (index >= entries_.size()) {
        return false;
    }
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

LoadResult LoadServers(const std::string& path, ServerList& list) {
    std::string content;
    const LoadResult read = ReadTextFile(path, kMaxReadBytes, content);
    if (read == LoadResult::Loaded) {
        list = ServerList::Parse(content);
    }
    return read; // (a file that is busy or not a file is not "missing": it is never seeded over)
}

bool SaveServers(const std::string& path, const ServerList& list) {
    const std::size_t slash = path.rfind('/');
    if (slash != std::string::npos) {
        mkdir(path.substr(0, slash).c_str(), 0777); // (the data folder is made when the app starts; this only covers a folder that went missing)
    }
    return WriteTextFile(path, list.Serialise());
}

} // namespace setup
