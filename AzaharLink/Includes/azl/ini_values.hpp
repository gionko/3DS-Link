// Values of AzaharLink.ini that the plugin and the Setup app both read and write. They live here, once, so that the
// two cannot disagree and so that the PC tests run the same code.
#pragma once

#include <cstdlib>
#include <string>
#include <vector>

namespace azl {
namespace ini {

/// The chord the in-game menu opens with when it is switched on and no chord of the user is known (Select is always part of any chord):
/// L, the D-pad's right and Select, beside Rosalina's L + down + Select. (L + left + Select is kept for the chat of the room.)
constexpr const char* kDefaultMenuKey = "L+Right+Select";
/// The default before 1.1.1, L+R+Select: a file that still holds exactly it never chose it, and takes the new default (MenuKeyFromFile).
/// L+R with Select is close to the soft reset of several games.
constexpr const char* kOldDefaultMenuKey = "L+R+Select";
/// The chord that opens the chat of the room in a title (ini key chat_key): L, the D-pad's left and Select, the mirror of the menu's.
constexpr const char* kDefaultChatKey = "L+Left+Select";
/// What menu_key holds when the in-game menu is switched off.
constexpr const char* kMenuKeyOff = "off";

/// The port of an Azahar room (the same number as room::kDefaultPort, which the Setup app checks with a static_assert).
constexpr int kPortDefault = 24872;
/// ini key `rcvbuf_kb`: the receive ring asked of the soc service. 64 KB is the hard maximum (more makes the service
/// fail or crash, see azl/rcvbuf.h); 8, 16 and 32 are fallbacks that can only lower it.
constexpr int kRcvbufKbDefault = 64;
/// ini key `sleep_stop`: THE place that says what a file without the key means, and what a new file contains (see SleepStop).
constexpr int kSleepStopDefault = 0;

/// ini key `chat_pos`: the corner of the top screen where the chat's lines show. The file holds a word (kChatPosWords), the
/// programs the index; the order is the one the app's row and the in-game menu cycle through.
enum ChatPos : int { kChatPosTopLeft = 0, kChatPosTopRight = 1, kChatPosBottomLeft = 2, kChatPosBottomRight = 3, kChatPosCount = 4 };
/// Bottom left, the user's choice (2026-11-04): what a new file holds and what a missing key or a word that is none of the four means.
constexpr int kChatPosDefault = kChatPosBottomLeft;
constexpr const char* kChatPosWords[kChatPosCount] = {"top-left", "top-right", "bottom-left", "bottom-right"};
constexpr const char* kChatPosLabels[kChatPosCount] = {"Top left", "Top right", "Bottom left", "Bottom right"};

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

/// ini key `chat_pos`: one of the four words (any case, blanks around). Anything else - an empty value, a typo, a number - is the
/// default: a slip must not move the chat over what the game shows elsewhere.
inline int ParseChatPos(const std::string& value) {
    const std::string word = Word(value);
    for (int i = 0; i < kChatPosCount; ++i) {
        if (word == kChatPosWords[i]) {
            return i;
        }
    }
    return kChatPosDefault;
}

/// A corner that is not one of the four (a field nobody parsed) is the default: the writer and the screens never index out of the tables.
inline int ValidChatPos(int pos) {
    return pos >= 0 && pos < kChatPosCount ? pos : kChatPosDefault;
}

/// The word the file holds, and the words the app and the in-game menu show.
inline const char* ChatPosWord(int pos) {
    return kChatPosWords[ValidChatPos(pos)];
}
inline const char* ChatPosLabel(int pos) {
    return kChatPosLabels[ValidChatPos(pos)];
}

/// The corner after `pos` (step >= 0: A or D-pad right) or before it (step < 0: D-pad left), round the four.
inline int NextChatPos(int pos, int step) {
    const int at = ValidChatPos(pos);
    return step < 0 ? (at + kChatPosCount - 1) % kChatPosCount : (at + 1) % kChatPosCount;
}

/// ini key `menu_key`: "off" or "none" (any case, spaces around are ignored) = the in-game menu never opens.
/// Anything else is a chord (see ParseKeys in Sources/main.cpp); an empty value means "not set" and is never
/// stored, so it is not "off" either.
inline bool MenuKeyIsOff(const std::string& value) {
    const std::string word = Word(value);
    return word == "off" || word == "none";
}

/// ini key `menu_key` as the plugin and the app take it from the file: the old default chord (any case, blanks around) becomes the
/// new one, every other value - a chord the user wrote, off - stays as it is.
inline std::string MenuKeyFromFile(const std::string& value) {
    return Word(value) == Word(kOldDefaultMenuKey) ? std::string(kDefaultMenuKey) : value;
}

/// The keys of a chord as the plugin takes them (ParseKeys in Sources/main.cpp: the twelve words in any case, '+', ' ' or ',' between
/// them, a word it does not know is no key), one bit a word, Select always among them (the plugin adds it to both chords).
inline unsigned ChordBits(const std::string& chord) {
    static const char* const kNames[] = {"select", "start", "a", "b", "x", "y", "l", "r", "up", "down", "left", "right"};
    unsigned bits = 1u; // (Select)
    std::string token;
    for (std::size_t i = 0; i <= chord.size(); ++i) {
        const char c = i < chord.size() ? chord[i] : '+';
        if (c == '+' || c == ' ' || c == ',') {
            for (unsigned k = 0; k < sizeof(kNames) / sizeof(kNames[0]); ++k) {
                if (token == kNames[k]) {
                    bits |= 1u << k;
                }
            }
            token.clear();
        } else {
            token += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }
    }
    return bits;
}

/// Two chords one of which holds every key of the other (the same chord too): the menu's and the chat's cannot be such a pair, since
/// holding the bigger completes the smaller on the way and the menu, looked at first, opens instead of the chat (or the chat, open,
/// keeps the menu shut). The plugin does the same test on its keys (Sources/main.cpp).
inline bool ChordsNested(const std::string& a, const std::string& b) {
    const unsigned x = ChordBits(a), y = ChordBits(b);
    return (x & y) == x || (x & y) == y;
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
/// exactly these 18 (Tests/policy_check.sh checks it), the 13 first are the keys of 0.1.14, chat and chat_key came with the in-game menu
/// of 1.1.1, chat_pos with the chat itself.
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
    std::string menu_key = kMenuKeyOff; // the in-game menu is off until the user switches it on
    std::string game_name;
    int debug = 0;
    int rcvbuf_kb = kRcvbufKbDefault;
    int sleep_stop = kSleepStopDefault;
    bool chat = true;                       // the chat of the room shown over the title (on: the user's choice; it can be switched off)
    std::string chat_key = kDefaultChatKey; // the chord that opens it
    int chat_pos = kChatPosDefault;         // the corner of its lines (ParseChatPos; the file holds the word)
};
constexpr int kKeyCount = 18;
constexpr const char* kKeyNames[kKeyCount] = {
    "enabled", "host",      "port",  "nickname",       "password",  "tunnel",    "autoconnect", "radio_like",
    "socket_buffers", "net_thread_core", "menu_key", "game_name", "debug", "rcvbuf_kb", "sleep_stop", "chat", "chat_key", "chat_pos"};

/// Values are single lines: a stray newline would corrupt the file.
inline std::string OneLine(std::string s) {
    for (char& c : s) {
        if (c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return s;
}

/// The whole file: all 18 keys, one per line, LF line ends, always the same order, then `extra` (lines the writer does not
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
    out += std::string("chat=") + flag(f.chat) + "\n";
    out += "chat_key=" + OneLine(f.chat_key) + "\n";
    out += std::string("chat_pos=") + ChatPosWord(f.chat_pos) + "\n";
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
