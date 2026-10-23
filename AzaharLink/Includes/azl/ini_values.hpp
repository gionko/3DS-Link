// Values of AzaharLink.ini that the plugin and the Setup app both read and write. They live here, once, so that the
// two cannot disagree and so that the PC tests run the same code.
#pragma once

#include <cstdlib>
#include <string>
#include <vector>

namespace azl {
namespace ini {

/// The chord that opens the in-game menu when the ini does not say (Select is always part of any chord).
constexpr const char* kDefaultMenuKey = "L+R+Select";
/// What menu_key holds when the in-game menu is switched off.
constexpr const char* kMenuKeyOff = "off";

/// The port of an Azahar room (the same number as room::kDefaultPort, which the Setup app checks with a static_assert).
constexpr int kPortDefault = 24872;
/// ini key `rcvbuf_kb`: the receive ring asked of the soc service. 64 KB is the hard maximum (more makes the service
/// fail or crash, see azl/rcvbuf.h); 8, 16 and 32 are fallbacks that can only lower it.
constexpr int kRcvbufKbDefault = 64;
/// ini key `sleep_stop`: THE place that says what a file without the key means, and what a new file contains (see SleepStop).
constexpr int kSleepStopDefault = 0;

/// ini key `debug`: 0 = no log of any kind (no file is written), 1 = everything (AzaharLink.log and
/// AzaharLink.debug.log). 0.1.14 and older also knew 2 (+ every IPC call): that is part of 1 now, so an old file
/// that says 2 is read as 1 and written back as 1. A negative number, or text that is not a number, is 0.
inline int DebugLevel(const std::string& value) {
    return std::strtol(value.c_str(), nullptr, 10) >= 1 ? 1 : 0;
}

/// The value without the blanks (and the CR of a CRLF file) around it, in lower case: how the words of the file are compared.
inline std::string Word(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r')) {
        --end;
    }
    std::string word;
    for (std::size_t i = begin; i < end; ++i) {
        const char c = value[i];
        word += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return word;
}

/// A switch of the file: 1/on/true/yes is on, 0/off/false/no is off (any case, blanks around). Anything else - an empty
/// value, a typo, "2" - is `fallback`, the default of that key (`known`, when given, says whether the value was one of the
/// words). The rule before was "anything but 0 is on", which turned a typo in a key that is off by default (sleep_stop=) ON
/// and left `enabled=off` ON.
inline bool ParseBool(const std::string& value, bool fallback, bool* known = nullptr) {
    const std::string word = Word(value);
    if (known) {
        *known = true;
    }
    if (word == "1" || word == "on" || word == "true" || word == "yes") {
        return true;
    }
    if (word == "0" || word == "off" || word == "false" || word == "no") {
        return false;
    }
    if (known) {
        *known = false;
    }
    return fallback;
}

/// ini key `radio_like`: 0 = everything through the room is reliable; 1 = beacons and broadcast game data go without
/// retries (the real radio's way); 2 = unicast game data too, sent twice to a console that numbers its frames (one with
/// this plugin: it drops the copy by the frame's number; to an Azahar or an older plugin the frame goes once and reliable),
/// and the room's reliable stream is taken as it arrives (RoomClient::SetFastData). The words of a switch mean 0 and 1
/// (the key was a switch until 2026-10-19); a slip is the default, 1.
inline int ParseRadioLike(const std::string& value) {
    if (Word(value) == "2") {
        return 2;
    }
    bool known = false;
    const bool on = ParseBool(value, true, &known);
    return known ? (on ? 1 : 0) : 1;
}

/// ini key `sleep_stop`: 0 = a sleep (lid closed) is only logged; 1 = it ends the game's session and the room link of a console
/// that is not hosting (a client, or not in a session); 2 = of a host too. The key was on or off before 0.1.16, so a file with
/// `sleep_stop=1` (or on/true/yes) still means 1; the words of "off" are 0. A value that is none of these - an empty one, a typo,
/// a 3 - is the default (0): the action ends a game session, so a slip must not turn it on.
inline int SleepStop(const std::string& value) {
    bool known = false;
    const bool on = ParseBool(value, false, &known);
    if (known) {
        return on ? 1 : 0;
    }
    return Word(value) == "2" ? 2 : kSleepStopDefault;
}

/// ini key `rcvbuf_kb`: carried as the file says it (a number the plugin refuses, such as 128, stays visible to it and to its
/// warning on the screen); text that is not a number from end to end is the default.
inline int RcvbufKb(const std::string& value) {
    const std::string text = Word(value);
    char* stop = nullptr;
    const long kb = std::strtol(text.c_str(), &stop, 10);
    return (stop != text.c_str() && *stop == '\0') ? static_cast<int>(kb) : kRcvbufKbDefault;
}

/// ini key `menu_key`: "off" or "none" (any case, spaces around are ignored) = the in-game menu never opens.
/// Anything else is a chord (see ParseKeys in Sources/main.cpp); an empty value means "not set" and is never
/// stored, so it is not "off" either.
inline bool MenuKeyIsOff(const std::string& value) {
    const std::string word = Word(value);
    return word == "off" || word == "none";
}

/// One line of the file as key and value. A UTF-8 byte order mark at the start (some Windows editors add one), the CR of a
/// CRLF file and blanks around the key, the '=' and the value are dropped. `ok` is false for a blank line, a line without
/// '=' (a comment, a line the file's owner wrote for himself) and an empty key.
struct KeyValue {
    bool ok = false;
    std::string key;
    std::string value;
};
inline KeyValue SplitLine(const std::string& line) {
    KeyValue kv;
    std::size_t begin = 0;
    std::size_t end = line.size();
    if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        begin = 3;
    }
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (begin < end && blank(line[begin])) {
        ++begin;
    }
    while (end > begin && blank(line[end - 1])) {
        --end;
    }
    const std::size_t eq = line.find('=', begin);
    if (eq == std::string::npos || eq >= end) {
        return kv;
    }
    std::size_t key_end = eq;
    while (key_end > begin && blank(line[key_end - 1])) {
        --key_end;
    }
    if (key_end == begin) {
        return kv;
    }
    std::size_t value_begin = eq + 1;
    while (value_begin < end && blank(line[value_begin])) {
        ++value_begin;
    }
    kv.ok = true;
    kv.key = line.substr(begin, key_end - begin);
    kv.value = line.substr(value_begin, end - value_begin);
    return kv;
}

/// Every key of AzaharLink.ini, in the order both programs write them, with the value a new file holds. The plugin reads
/// exactly these 15 (Tests/policy_check.sh checks it) and the 13 first are the keys of 0.1.14.
struct Fields {
    bool enabled = true;
    std::string host;
    int port = kPortDefault;
    std::string nickname;
    std::string password;
    bool tunnel = true;
    bool autoconnect = true;
    int radio_like = 1; // 0, 1, 2 (ParseRadioLike)
    bool socket_buffers = true;
    int net_thread_core = -1;
    std::string menu_key = kDefaultMenuKey;
    std::string game_name;
    int debug = 0;
    int rcvbuf_kb = kRcvbufKbDefault;
    int sleep_stop = kSleepStopDefault;
};
constexpr int kKeyCount = 15;
constexpr const char* kKeyNames[kKeyCount] = {
    "enabled", "host",      "port",  "nickname",       "password",  "tunnel",    "autoconnect", "radio_like",
    "socket_buffers", "net_thread_core", "menu_key", "game_name", "debug", "rcvbuf_kb", "sleep_stop"};

/// Values are single lines: a stray newline would corrupt the file.
inline std::string OneLine(std::string s) {
    for (char& c : s) {
        if (c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return s;
}

/// The whole file: all 15 keys, one per line, LF line ends, always the same order, then `extra` (lines the writer does not
/// understand and keeps) as far as they fit in `max_bytes`. The plugin refuses a file larger than 4096 bytes.
inline std::string Render(const Fields& f, const std::vector<std::string>& extra = std::vector<std::string>(),
                          std::size_t max_bytes = 3800) {
    const auto flag = [](bool b) { return b ? "1" : "0"; };
    std::string out;
    out += std::string("enabled=") + flag(f.enabled) + "\n";
    out += "host=" + OneLine(f.host) + "\n";
    out += "port=" + std::to_string(f.port) + "\n";
    out += "nickname=" + OneLine(f.nickname) + "\n";
    out += "password=" + OneLine(f.password) + "\n";
    out += std::string("tunnel=") + flag(f.tunnel) + "\n";
    out += std::string("autoconnect=") + flag(f.autoconnect) + "\n";
    out += "radio_like=" + std::to_string(f.radio_like) + "\n";
    out += std::string("socket_buffers=") + flag(f.socket_buffers) + "\n";
    out += "net_thread_core=" + std::to_string(f.net_thread_core) + "\n";
    out += "menu_key=" + OneLine(f.menu_key) + "\n";
    out += "game_name=" + OneLine(f.game_name) + "\n";
    out += "debug=" + std::to_string(f.debug) + "\n";
    out += "rcvbuf_kb=" + std::to_string(f.rcvbuf_kb) + "\n";
    out += "sleep_stop=" + std::to_string(f.sleep_stop) + "\n";
    for (const std::string& line : extra) {
        if (out.size() + line.size() + 1 > max_bytes) {
            break;
        }
        out += OneLine(line) + "\n";
    }
    return out;
}

} // namespace ini
} // namespace azl
