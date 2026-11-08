// The rooms the in-game menu offers to switch to: the user's saved rooms (servers.txt, the app's Server list) and the public
// lobbies (public-lobbies.txt, with the names and passwords the user gave some of them in public-lobbies-local.txt), and what the
// app measured of them (public-lobbies-ping.txt). The files are written by the app; the plugin only reads them, and only while the
// menu shows that list. It keeps the text of the list (at most kMaxListBytes) and 8 bytes a room (where its line is; its ping,
// players and slots); names and addresses are never copied - a room is read out of the text when it is drawn or picked, and the
// order compares the names where they are in the text. Everything goes when the list closes. Plain C++ (Tests/room_choices_test.cpp).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace azl {
namespace rooms {

/// The port of a saved room without one (ini::kPortDefault, the room's default).
constexpr std::uint16_t kDefaultPort = 24872;
/// The most read of each file (a bigger one is read up to its last whole line in that much): the list (about 270 rooms; today's has
/// 80 in 5 KB), the user's names and passwords, the app's pings (read, applied as 4 bytes a room, and let go).
constexpr std::size_t kMaxListBytes = 16 * 1024;
constexpr std::size_t kMaxLocalBytes = 4 * 1024;
constexpr std::size_t kMaxPingBytes = 8 * 1024;
/// The most rooms of a list (an index of the order is 2 bytes).
constexpr std::size_t kMaxRooms = 300;

/// What the app measured of a room the last time its page "Public lobbies" asked the rooms (public-lobbies-ping.txt, written by the app
/// at the end of every pass: "#measured <seconds since 1970>", then "address:port:ping:players:slots", ping -1 = offline).
struct Seen {
    std::int16_t ping = -2; ///< -2 not in the file, -1 offline, else the ping in ms
    std::uint8_t players = 0;
    std::uint8_t slots = 0;   ///< 0 = not known
};

/// The order of a list: by the ping the app measured (rooms with a free slot first, the lower ping first, then the fuller room; the ones
/// not measured, the full ones, the offline ones after), or by name.
enum class SortBy { Ping, Name };

struct Choice {
    std::string name; ///< what the menu shows ("" = the address)
    std::string host;
    std::uint16_t port = 0;
    std::string password; ///< "" = none known
};

/// A piece of a text, not a copy of it.
struct Span {
    const char* p = nullptr;
    std::size_t n = 0;
};

inline char LowerChar(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool SameNoCase(const Span& a, const Span& b) {
    if (a.n != b.n) {
        return false;
    }
    for (std::size_t i = 0; i < a.n; ++i) {
        if (LowerChar(a.p[i]) != LowerChar(b.p[i])) {
            return false;
        }
    }
    return true;
}

inline int CompareNoCase(const Span& a, const Span& b) {
    const std::size_t n = a.n < b.n ? a.n : b.n;
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char x = static_cast<unsigned char>(LowerChar(a.p[i]));
        const unsigned char y = static_cast<unsigned char>(LowerChar(b.p[i]));
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return a.n == b.n ? 0 : (a.n < b.n ? -1 : 1);
}

inline bool PortSpan(const Span& s, std::uint16_t& out) {
    if (s.n == 0 || s.n > 5) {
        return false;
    }
    unsigned long value = 0;
    for (std::size_t i = 0; i < s.n; ++i) {
        if (s.p[i] < '0' || s.p[i] > '9') {
            return false;
        }
        value = value * 10 + static_cast<unsigned long>(s.p[i] - '0');
    }
    if (value == 0 || value > 65535) {
        return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

inline bool HostSpan(const Span& s) {
    if (s.n == 0 || s.n > 80) {
        return false;
    }
    for (std::size_t i = 0; i < s.n; ++i) {
        const char c = s.p[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/// The line of `text` that begins at `pos` (without its CR); `next` is where the following one begins.
inline Span LineAt(const std::string& text, std::size_t pos, std::size_t& next) {
    std::size_t end = text.find('\n', pos);
    next = end == std::string::npos ? text.size() : end + 1;
    if (end == std::string::npos) {
        end = text.size();
    }
    Span s;
    s.p = text.data() + pos;
    s.n = end - pos;
    while (s.n > 0 && (s.p[s.n - 1] == '\r')) {
        --s.n;
    }
    return s;
}

/// The address, the port, the name and the password of a line, as pieces of it: "host[:port][|name[|password]]" (servers.txt; the
/// password is everything after the second bar) or "address:port:name[:secret]" (the public lists; the name may be empty only when
/// `allow_empty_name`, the user's own file).
struct Fields {
    Span host, name, password;
    std::uint16_t port = 0;
};

inline bool SavedFields(Span line, Fields& f) {
    while (line.n > 0 && line.p[line.n - 1] == ' ') {
        --line.n;
    }
    std::size_t bar = 0;
    while (bar < line.n && line.p[bar] != '|') {
        ++bar;
    }
    std::size_t colon = bar;
    while (colon > 0 && line.p[colon - 1] != ':') {
        --colon;
    }
    f = Fields();
    f.port = kDefaultPort;
    if (colon > 0) { // ("host:port")
        f.host = Span{line.p, colon - 1};
        if (!PortSpan(Span{line.p + colon, bar - colon}, f.port)) {
            return false;
        }
    } else {
        f.host = Span{line.p, bar};
    }
    if (!HostSpan(f.host)) {
        return false;
    }
    if (bar < line.n) {
        std::size_t bar2 = bar + 1;
        while (bar2 < line.n && line.p[bar2] != '|') {
            ++bar2;
        }
        f.name = Span{line.p + bar + 1, bar2 - bar - 1};
        if (bar2 < line.n) {
            f.password = Span{line.p + bar2 + 1, line.n - bar2 - 1};
        }
    }
    return true;
}

inline bool PublicFields(const Span& line, Fields& f, bool allow_empty_name) {
    std::size_t c[3];
    int found = 0;
    for (std::size_t i = 0; i < line.n && found < 3; ++i) {
        if (line.p[i] == ':') {
            c[found++] = i;
        }
    }
    if (found < 2) {
        return false;
    }
    f = Fields();
    f.host = Span{line.p, c[0]};
    if (!HostSpan(f.host) || !PortSpan(Span{line.p + c[0] + 1, c[1] - c[0] - 1}, f.port)) {
        return false;
    }
    const std::size_t name_end = found == 3 ? c[2] : line.n;
    f.name = Span{line.p + c[1] + 1, name_end - c[1] - 1};
    if (found == 3) {
        f.password = Span{line.p + c[2] + 1, line.n - c[2] - 1};
    }
    return f.name.n > 0 || allow_empty_name;
}

inline std::string Text(const Span& s) {
    std::string out(s.p ? s.p : "", s.n);
    for (char& ch : out) {
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) {
            ch = ' ';
        }
    }
    return out;
}

/// Whole-string forms of the two line formats (the tests; the menu reads the lines in place).
inline bool ParseSavedLine(const std::string& line, Choice& out) {
    std::size_t next = 0;
    Fields f;
    if (!SavedFields(LineAt(line, 0, next), f)) {
        return false;
    }
    out.host = Text(f.host);
    out.port = f.port;
    out.name = Text(f.name);
    out.password = std::string(f.password.p ? f.password.p : "", f.password.n);
    return true;
}

inline bool ParsePublicLine(const std::string& line, Choice& out, bool allow_empty_name) {
    std::size_t next = 0;
    Fields f;
    if (!PublicFields(LineAt(line, 0, next), f, allow_empty_name)) {
        return false;
    }
    out.host = Text(f.host);
    out.port = f.port;
    out.name = Text(f.name);
    out.password = std::string(f.password.p ? f.password.p : "", f.password.n);
    return true;
}

inline bool SameHost(const std::string& a, const std::string& b) {
    return SameNoCase(Span{a.data(), a.size()}, Span{b.data(), b.size()});
}

/// One of the two lists, as the menu holds it while it is shown.
class RoomFile {
public:
    enum class Kind { Saved, Public };

    /// Takes the text of the file (and, for the public lobbies, of the user's file) and finds its rooms: the other lines (comments,
    /// a line that is not a room) are skipped. At most `max_rooms`.
    void Load(Kind kind, std::string text, std::string local_text = std::string(), std::size_t max_rooms = kMaxRooms) {
        Clear();
        kind_ = kind;
        text_.swap(text);
        local_.swap(local_text);
        if (max_rooms > kMaxRooms) {
            max_rooms = kMaxRooms;
        }
        std::size_t pos = 0;
        while (pos < text_.size() && lines_.size() < max_rooms) {
            std::size_t next = 0;
            const Span line = LineAt(text_, pos, next);
            Fields f;
            if (line.n > 0 && line.p[0] != '#' && ParseFields(line, f)) {
                lines_.push_back(static_cast<std::uint32_t>(pos));
            }
            pos = next;
        }
    }

    /// Gives the memory back (the swap: clear() keeps the capacity).
    void Clear() {
        std::string().swap(text_);
        std::string().swap(local_);
        std::vector<std::uint32_t>().swap(lines_);
        std::vector<Seen>().swap(seen_);
        measured_at_ = 0;
    }

    std::size_t size() const {
        return lines_.size();
    }

    /// Room `i` of the list. A public room takes the name and the password the user gave it, when there are.
    bool Get(std::size_t i, Choice& out) const {
        Fields f;
        if (!RoomFields(i, f)) {
            return false;
        }
        out.host = Text(f.host);
        out.port = f.port;
        out.name = Text(f.name);
        out.password = std::string(f.password.p ? f.password.p : "", f.password.n);
        return true;
    }

    /// The text of public-lobbies-ping.txt: what the app measured, room by room (4 bytes a room; the text itself is not kept).
    void ApplyPings(const std::string& text) {
        seen_.assign(lines_.size(), Seen());
        measured_at_ = 0;
        static const char kHead[] = "#measured ";
        constexpr std::size_t kHeadLen = sizeof(kHead) - 1;
        std::size_t pos = 0;
        while (pos < text.size()) {
            std::size_t next = 0;
            const Span line = LineAt(text, pos, next);
            pos = next;
            if (line.n > kHeadLen && std::string(line.p, kHeadLen) == kHead) {
                measured_at_ = std::strtoull(std::string(line.p + kHeadLen, line.n - kHeadLen).c_str(), nullptr, 10);
                continue;
            }
            // address:port:ping:players:slots
            std::size_t c[4];
            int found = 0;
            for (std::size_t k = 0; k < line.n && found < 4; ++k) {
                if (line.p[k] == ':') {
                    c[found++] = k;
                }
            }
            std::uint16_t port = 0;
            if (found < 4 || !PortSpan(Span{line.p + c[0] + 1, c[1] - c[0] - 1}, port)) {
                continue;
            }
            const Span host{line.p, c[0]};
            const long ping = Number(Span{line.p + c[1] + 1, c[2] - c[1] - 1});
            const long players = Number(Span{line.p + c[2] + 1, c[3] - c[2] - 1});
            const long slots = Number(Span{line.p + c[3] + 1, line.n - c[3] - 1});
            for (std::size_t i = 0; i < lines_.size(); ++i) {
                Fields f;
                if (ListFields(i, f) && f.port == port && SameNoCase(f.host, host)) {
                    seen_[i].ping = static_cast<std::int16_t>(ping < 0 ? -1 : ping > 9999 ? 9999 : ping);
                    seen_[i].players = static_cast<std::uint8_t>(players < 0 ? 0 : players > 255 ? 255 : players);
                    seen_[i].slots = static_cast<std::uint8_t>(slots < 0 ? 0 : slots > 255 ? 255 : slots);
                    break;
                }
            }
        }
    }

    /// The room of the list at host:port, or size() when it is not in it.
    std::size_t Find(const std::string& host, std::uint16_t port) const {
        const Span want{host.data(), host.size()};
        for (std::size_t i = 0; i < lines_.size(); ++i) {
            Fields f;
            if (ListFields(i, f) && f.port == port && SameNoCase(f.host, want)) {
                return i;
            }
        }
        return lines_.size();
    }

    /// What is known of room `i` now (the room 3DS Link is in: what the room says this moment), in place of what the app measured.
    void SetSeen(std::size_t i, const Seen& s) {
        if (i < seen_.size()) {
            seen_[i] = s;
        }
    }

    /// When the app measured the pings (seconds since 1970; 0 = no file).
    std::uint64_t measured_at() const {
        return measured_at_;
    }

    /// What the app measured of room `i`.
    Seen seen(std::size_t i) const {
        return i < seen_.size() ? seen_[i] : Seen();
    }

    /// Puts the rooms in the order asked. Memory: 2 bytes a room for the order and, while it is made, 8 bytes a room for where each
    /// name is in the text; the names are compared where they are.
    void Sort(SortBy by) {
        const std::size_t n = lines_.size();
        std::vector<std::uint16_t> order(n);
        std::vector<Span> names(n);
        for (std::size_t i = 0; i < n; ++i) {
            order[i] = static_cast<std::uint16_t>(i);
            Fields f;
            if (RoomFields(i, f)) {
                names[i] = f.name.n > 0 ? f.name : f.host;
            }
        }
        const auto group = [](const Seen& s) {
            if (s.ping >= 0) {
                return (s.slots > 0 && s.players >= s.slots) ? 2 : 0;
            }
            return s.ping == -1 ? 3 : 1;
        };
        std::stable_sort(order.begin(), order.end(), [&](std::uint16_t a, std::uint16_t b) {
            if (by == SortBy::Ping) {
                const Seen sa = seen(a), sb = seen(b);
                const int ga = group(sa), gb = group(sb);
                if (ga != gb) {
                    return ga < gb;
                }
                if ((ga == 0 || ga == 2) && sa.ping != sb.ping) {
                    return sa.ping < sb.ping;
                }
                if ((ga == 0 || ga == 2) && sa.players != sb.players) {
                    return sa.players > sb.players;
                }
            }
            return CompareNoCase(names[a], names[b]) < 0;
        });
        std::vector<Span>().swap(names);
        std::vector<std::uint32_t> lines(n);
        for (std::size_t i = 0; i < n; ++i) {
            lines[i] = lines_[order[i]];
        }
        lines_.swap(lines);
        if (seen_.size() == n) {
            std::vector<Seen> seen(n);
            for (std::size_t i = 0; i < n; ++i) {
                seen[i] = seen_[order[i]];
            }
            seen_.swap(seen);
        }
    }

private:
    static long Number(const Span& s) {
        long v = 0;
        bool neg = false;
        std::size_t i = 0;
        if (i < s.n && s.p[i] == '-') {
            neg = true;
            ++i;
        }
        for (; i < s.n && s.p[i] >= '0' && s.p[i] <= '9' && v < 100000; ++i) {
            v = v * 10 + (s.p[i] - '0');
        }
        return neg ? -v : v;
    }

    bool ParseFields(const Span& line, Fields& f) const {
        return kind_ == Kind::Saved ? SavedFields(line, f) : PublicFields(line, f, false);
    }

    // The fields of room `i` as the list has them.
    bool ListFields(std::size_t i, Fields& f) const {
        if (i >= lines_.size()) {
            return false;
        }
        std::size_t next = 0;
        return ParseFields(LineAt(text_, lines_[i], next), f);
    }

    // ... with the name and the password the user gave the room (a public one), when there are.
    bool RoomFields(std::size_t i, Fields& f) const {
        if (!ListFields(i, f)) {
            return false;
        }
        if (kind_ != Kind::Public) {
            return true;
        }
        std::size_t pos = 0;
        while (pos < local_.size()) {
            std::size_t next = 0;
            const Span line = LineAt(local_, pos, next);
            pos = next;
            Fields own;
            if (line.n > 0 && line.p[0] != '#' && PublicFields(line, own, true) && own.port == f.port && SameNoCase(own.host, f.host)) {
                if (own.name.n > 0) {
                    f.name = own.name;
                }
                if (own.password.n > 0) {
                    f.password = own.password;
                }
                break;
            }
        }
        return true;
    }

    Kind kind_ = Kind::Saved;
    std::string text_;
    std::string local_;
    std::vector<std::uint32_t> lines_;
    std::vector<Seen> seen_;          // (parallel to lines_; empty until ApplyPings)
    std::uint64_t measured_at_ = 0;
};

} // namespace rooms
} // namespace azl
