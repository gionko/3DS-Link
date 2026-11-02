// The public room list of an Azahar lobby server: GET <web api url>/lobby answers {"rooms":[{...}, ...]} (the same JSON that Azahar's own
// lobby client reads, see web_service/announce_room_json.cpp). This is the small reader of it: no library, no exceptions, no allocation
// beyond the strings of the rooms it keeps. It looks only at the keys 3DS Link needs and skips every other value, so a server that adds
// a field does not break it. Pure C++ (no 3ds.h): Tests/lobby_json_test.cpp runs it on a PC.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lobby {

struct Room {
    std::string name;
    std::string description;
    std::string address;        // the IP (or name) of the room's server
    std::string preferred_game; // "Monster Hunter", "" when the room has none
    unsigned port = 0;
    unsigned max_players = 0;
    unsigned net_version = 0;
    unsigned players = 0;                // how many members the server lists
    std::vector<std::string> nicknames;  // the first few of them
    bool has_password = false;
    bool active = true;
};

namespace detail {

class Reader {
public:
    Reader(const char* p, const char* end) : p_(p), end_(end) {}

    void Ws() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) {
            ++p_;
        }
    }
    bool Eof() const {
        return p_ >= end_;
    }
    bool Peek(char c) {
        Ws();
        return p_ < end_ && *p_ == c;
    }
    bool Take(char c) {
        if (Peek(c)) {
            ++p_;
            return true;
        }
        return false;
    }

    // A string; `out` may be null (the string is skipped). \uXXXX becomes UTF-8 (surrogate pairs too).
    bool String(std::string* out) {
        if (!Take('"')) {
            return false;
        }
        if (out) {
            out->clear();
        }
        while (p_ < end_) {
            const char c = *p_++;
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                if (out && out->size() < kMaxString) {
                    out->push_back(c);
                }
                continue;
            }
            if (p_ >= end_) {
                return false;
            }
            const char e = *p_++;
            unsigned cp = 0;
            switch (e) {
            case '"': cp = '"'; break;
            case '\\': cp = '\\'; break;
            case '/': cp = '/'; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'n': cp = '\n'; break;
            case 'r': cp = '\r'; break;
            case 't': cp = '\t'; break;
            case 'u': {
                if (!Hex4(cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                    const char* save = p_;
                    p_ += 2;
                    unsigned lo = 0;
                    if (Hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        p_ = save;
                    }
                }
                break;
            }
            default:
                return false;
            }
            if (out && out->size() < kMaxString) {
                Utf8(*out, cp);
            }
        }
        return false; // no closing quote
    }

    bool Number(double* out) {
        Ws();
        const char* start = p_;
        if (p_ < end_ && (*p_ == '-' || *p_ == '+')) {
            ++p_;
        }
        bool digits = false;
        while (p_ < end_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' || *p_ == 'E' || *p_ == '-' || *p_ == '+')) {
            digits = digits || (*p_ >= '0' && *p_ <= '9');
            ++p_;
        }
        if (!digits) {
            p_ = start;
            return false;
        }
        if (out) {
            // (no strtod: the value is a small count or a port; a huge id is read for its size only)
            double v = 0;
            bool neg = false;
            const char* q = start;
            if (*q == '-') { neg = true; ++q; } else if (*q == '+') { ++q; }
            while (q < p_ && *q >= '0' && *q <= '9') {
                v = v * 10 + (*q - '0');
                ++q;
            }
            *out = neg ? -v : v;
        }
        return true;
    }

    bool Literal(const char* word, std::size_t n) {
        Ws();
        if (static_cast<std::size_t>(end_ - p_) < n) {
            return false;
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (p_[i] != word[i]) {
                return false;
            }
        }
        p_ += n;
        return true;
    }
    bool Bool(bool* out) {
        if (Literal("true", 4)) {
            if (out) *out = true;
            return true;
        }
        if (Literal("false", 5)) {
            if (out) *out = false;
            return true;
        }
        return false;
    }

    // Skips any value (the depth is limited: a hostile reply cannot run the stack out).
    bool Skip(int depth = 0) {
        if (depth > 24) {
            return false;
        }
        Ws();
        if (p_ >= end_) {
            return false;
        }
        const char c = *p_;
        if (c == '"') {
            return String(nullptr);
        }
        if (c == '{') {
            ++p_;
            if (Take('}')) {
                return true;
            }
            for (;;) {
                if (!String(nullptr) || !Take(':') || !Skip(depth + 1)) {
                    return false;
                }
                if (Take(',')) {
                    continue;
                }
                return Take('}');
            }
        }
        if (c == '[') {
            ++p_;
            if (Take(']')) {
                return true;
            }
            for (;;) {
                if (!Skip(depth + 1)) {
                    return false;
                }
                if (Take(',')) {
                    continue;
                }
                return Take(']');
            }
        }
        if (Literal("null", 4)) {
            return true;
        }
        if (Bool(nullptr)) {
            return true;
        }
        return Number(nullptr);
    }

    static constexpr std::size_t kMaxString = 400; // a name or a description longer than this is cut: the screen shows 12 characters of it

private:
    bool Hex4(unsigned& out) {
        if (end_ - p_ < 4) {
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p_++;
            unsigned d;
            if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
            else return false;
            out = (out << 4) | d;
        }
        return true;
    }
    static void Utf8(std::string& s, unsigned cp) {
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    const char* p_;
    const char* end_;
};

inline bool ParsePlayers(Reader& r, Room& room) {
    if (!r.Take('[')) {
        return r.Skip(); // (not an array: ignored)
    }
    if (r.Take(']')) {
        return true;
    }
    for (;;) {
        if (r.Take('{')) {
            ++room.players;
            if (!r.Take('}')) {
                for (;;) {
                    std::string key;
                    if (!r.String(&key) || !r.Take(':')) {
                        return false;
                    }
                    if (key == "nickname") {
                        std::string nick;
                        if (!r.String(&nick)) {
                            return false;
                        }
                        if (room.nicknames.size() < 6 && !nick.empty()) {
                            room.nicknames.push_back(nick.substr(0, 24));
                        }
                    } else if (!r.Skip(2)) {
                        return false;
                    }
                    if (r.Take(',')) {
                        continue;
                    }
                    if (!r.Take('}')) {
                        return false;
                    }
                    break;
                }
            }
        } else if (!r.Skip(2)) {
            return false;
        }
        if (r.Take(',')) {
            continue;
        }
        return r.Take(']');
    }
}

inline bool ParseRoom(Reader& r, Room& room) {
    if (!r.Take('{')) {
        return false;
    }
    if (r.Take('}')) {
        return true;
    }
    for (;;) {
        std::string key;
        if (!r.String(&key) || !r.Take(':')) {
            return false;
        }
        double num = 0;
        bool ok = true;
        if (key == "name") {
            ok = r.Peek('"') ? r.String(&room.name) : r.Skip(2);
        } else if (key == "description") {
            ok = r.Peek('"') ? r.String(&room.description) : r.Skip(2);
        } else if (key == "address") {
            ok = r.Peek('"') ? r.String(&room.address) : r.Skip(2);
        } else if (key == "preferredGameName") {
            ok = r.Peek('"') ? r.String(&room.preferred_game) : r.Skip(2);
        } else if (key == "port") {
            ok = r.Number(&num);
            room.port = (num >= 0 && num <= 65535) ? static_cast<unsigned>(num) : 0;
        } else if (key == "maxPlayers") {
            ok = r.Number(&num);
            room.max_players = (num >= 0 && num < 100000) ? static_cast<unsigned>(num) : 0;
        } else if (key == "netVersion") {
            ok = r.Number(&num);
            room.net_version = (num >= 0 && num < 100000) ? static_cast<unsigned>(num) : 0;
        } else if (key == "hasPassword") {
            ok = r.Bool(&room.has_password);
        } else if (key == "active") {
            ok = r.Bool(&room.active);
        } else if (key == "players") {
            ok = ParsePlayers(r, room);
        } else {
            ok = r.Skip(2);
        }
        if (!ok) {
            return false;
        }
        if (r.Take(',')) {
            continue;
        }
        return r.Take('}');
    }
}

} // namespace detail

// Reads the reply of GET /lobby. Returns true when it was a list of rooms (it may be empty). Rooms that are not active, or have no address or
// no port, are left out; at most `max_rooms` are kept. `error` (when given) says what was wrong.
inline bool Parse(const std::string& json, std::vector<Room>& out, std::string* error = nullptr, std::size_t max_rooms = 300) {
    out.clear();
    detail::Reader r(json.data(), json.data() + json.size());
    if (!r.Take('{')) {
        if (error) *error = "The server did not answer with a room list.";
        return false;
    }
    bool found = false;
    if (!r.Take('}')) {
        for (;;) {
            std::string key;
            if (!r.String(&key) || !r.Take(':')) {
                if (error) *error = "The room list is damaged.";
                return false;
            }
            if (key == "rooms") {
                if (!r.Take('[')) {
                    if (error) *error = "The room list is not a list.";
                    return false;
                }
                found = true;
                if (!r.Take(']')) {
                    for (;;) {
                        Room room;
                        if (!detail::ParseRoom(r, room)) {
                            if (error) *error = "The room list is damaged.";
                            return false;
                        }
                        if (room.active && !room.address.empty() && room.port != 0 && out.size() < max_rooms) {
                            out.push_back(room);
                        }
                        if (r.Take(',')) {
                            continue;
                        }
                        if (!r.Take(']')) {
                            if (error) *error = "The room list is damaged.";
                            return false;
                        }
                        break;
                    }
                }
            } else if (!r.Skip(1)) {
                if (error) *error = "The room list is damaged.";
                return false;
            }
            if (r.Take(',')) {
                continue;
            }
            if (!r.Take('}')) {
                if (error) *error = "The room list is damaged.";
                return false;
            }
            break;
        }
    }
    if (!found && error) {
        *error = "The server has no \"rooms\" in its answer.";
    }
    return found;
}

// The text of the public room list for the plugin menu (public_rooms.txt): one room a line, host:port|<players>/<max> <name>, with a
// trailing |? when the room has a password. The plugin does no HTTP in the game, so the app writes the file after each refresh.
// Rooms that are not active, or whose address is not a valid host, are left out.
inline std::string PublicRoomsText(const std::vector<Room>& rooms, std::size_t max_rooms = 300) {
    std::string out;
    std::size_t lines = 0;
    for (const Room& r : rooms) {
        if (lines >= max_rooms) {
            break;
        }
        if (!r.active || r.address.empty() || r.address.size() > 100 || r.port == 0 || r.port > 65535) {
            continue;
        }
        bool host_ok = true;
        for (unsigned char c : r.address) {
            host_ok = host_ok && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_');
        }
        if (!host_ok) {
            continue;
        }
        std::string name;
        for (unsigned char c : r.name) {
            if (c >= 32 && c < 127 && c != '|') {
                name += static_cast<char>(c);
            } else if (c == '|' || c < 32) {
                name += ' ';
            } else if (c >= 0xC0) {
                name += '?'; // (a letter outside ASCII, whatever its length in UTF-8: the game's font cannot draw it, but the name keeps its length)
            }
        }
        std::string label = std::to_string(r.players) + "/" + std::to_string(r.max_players) + " " + name;
        if (label.size() > 100) {
            label.resize(100); // (the plugin keeps 100 characters of a name: a lobby name is at most 20-60)
        }
        out += r.address + ":" + std::to_string(r.port) + "|" + label;
        if (r.has_password) {
            out += "|?";
        }
        out += "\n";
        ++lines;
    }
    return out;
}

} // namespace lobby
