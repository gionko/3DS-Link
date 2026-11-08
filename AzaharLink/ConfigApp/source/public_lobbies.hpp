// The public lobbies: what the page "Public lobbies" decides, in plain C++ (no libctru: Tests/settings_test/public_lobbies_test.cpp builds it on a PC).
//
// The list comes from GitHub (gionko/3DS-Link-Public-Lobbies, made twice a day from the Azahar lobbies and our own lines), one room a line:
// "address:port:name" or "address:port:name:secret" (the name has no colon, the secret is everything after the third one). The user's own names
// and passwords are in a file of their own, so a new list can never take them away, and a room that left the list is dropped with them, except
// the room in use: it stays until the list changes once more. titles.txt says, for every title, its region and its game ("family").
//
// Each room is then asked by lobby_probe.cpp, without joining it: the ping, the members and what they play, a password, a full room.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace lobbies {

constexpr const char* kListUrl = "https://raw.githubusercontent.com/gionko/3DS-Link-Public-Lobbies/main/public-lobbies.txt";
constexpr const char* kTitlesUrl = "https://raw.githubusercontent.com/gionko/3DS-Link-Public-Lobbies/main/titles.txt";
constexpr std::size_t kMaxListBytes = 64 * 1024;
constexpr std::size_t kMaxLocalBytes = 64 * 1024;
constexpr std::size_t kMaxTitlesBytes = 128 * 1024;
constexpr std::size_t kMaxNameChars = 64;
constexpr std::size_t kMaxHostChars = 80;
constexpr std::size_t kMaxSecretChars = 64;

// ---------------------------------------------------------------------------------------------
// Lines and files
// ---------------------------------------------------------------------------------------------

struct Room {
    std::string host;
    std::uint16_t port = 0;
    std::string name;
    std::string secret;
};

inline std::string Lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

inline bool SameRoom(const std::string& host_a, std::uint16_t port_a, const std::string& host_b, std::uint16_t port_b) {
    return port_a == port_b && Lower(host_a) == Lower(host_b);
}

inline bool SameRoom(const Room& a, const Room& b) {
    return SameRoom(a.host, a.port, b.host, b.port);
}

inline std::string RoomKey(const Room& r) {
    return Lower(r.host) + ":" + std::to_string(r.port);
}

// "address:port:name[:secret]". The name may be empty only when `allow_empty_name` (the user's own file: a line that only keeps a password).
inline bool ParseRoomLine(std::string line, Room& out, bool allow_empty_name = false) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    const std::size_t c1 = line.find(':');
    if (c1 == std::string::npos) {
        return false;
    }
    const std::size_t c2 = line.find(':', c1 + 1);
    if (c2 == std::string::npos) {
        return false;
    }
    const std::size_t c3 = line.find(':', c2 + 1);
    Room room;
    room.host = line.substr(0, c1);
    const std::string port = line.substr(c1 + 1, c2 - c1 - 1);
    room.name = c3 == std::string::npos ? line.substr(c2 + 1) : line.substr(c2 + 1, c3 - c2 - 1);
    room.secret = c3 == std::string::npos ? std::string() : line.substr(c3 + 1);
    if (room.host.empty() || room.host.size() > kMaxHostChars || port.empty() || port.size() > 5) {
        return false;
    }
    for (char c : room.host) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-';
        if (!ok) {
            return false;
        }
    }
    unsigned long value = 0;
    for (char c : port) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<unsigned long>(c - '0');
    }
    if (value == 0 || value > 65535) {
        return false;
    }
    room.port = static_cast<std::uint16_t>(value);
    for (char& c : room.name) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            c = ' ';
        }
    }
    if (room.name.size() > kMaxNameChars * 4) { // (64 letters of UTF-8 at most)
        room.name.resize(kMaxNameChars * 4);
    }
    if ((room.name.empty() && !allow_empty_name) || room.secret.size() > kMaxSecretChars * 4) { // (64 letters of UTF-8 at most, as the keyboard counts them)
        return false;
    }
    out = room;
    return true;
}

inline std::string RoomLine(const Room& r) {
    return r.host + ":" + std::to_string(r.port) + ":" + r.name + (r.secret.empty() ? std::string() : ":" + r.secret);
}

inline std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(line);
        pos = end + 1;
    }
    return out;
}

// The value of "#word value" in a header line ("" when the text has none).
inline std::string HeaderValue(const std::string& text, const std::string& word) {
    for (const std::string& line : Lines(text)) {
        const std::string head = "#" + word + " ";
        if (line.compare(0, head.size(), head) == 0) {
            return line.substr(head.size());
        }
    }
    return std::string();
}

// public-lobbies.txt.
struct List {
    std::string updated;      // "#updated YYYY-MM-DD": changes with every new list (see Remember)
    std::string titles_stamp; // "#titles ...": titles.txt is fetched again when it changes
    std::vector<Room> rooms;
};

// False when the text is not a list: no "#updated" line, or no room at all (an HTML page, a cut answer). A room twice is kept once.
inline bool ParseList(const std::string& text, List& out) {
    List list;
    list.updated = HeaderValue(text, "updated");
    list.titles_stamp = HeaderValue(text, "titles");
    std::set<std::string> seen;
    for (const std::string& line : Lines(text)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        Room room;
        if (ParseRoomLine(line, room) && seen.insert(RoomKey(room)).second) {
            list.rooms.push_back(room);
        }
    }
    if (list.updated.empty() || list.rooms.empty()) {
        return false;
    }
    out = list;
    return true;
}

// What a list is, for "the list changed": its rooms, not its date (the date is renewed when nothing changed for a month, and two lists of one day
// have the same date). 16 hex digits of FNV-1a over the room lines.
inline std::string ListId(const List& list) {
    std::uint64_t h = 1469598103934665603ull;
    for (const Room& r : list.rooms) {
        for (unsigned char c : RoomLine(r) + "\n") {
            h = (h ^ c) * 1099511628211ull;
        }
    }
    static const char* digits = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i, h >>= 4) {
        out[static_cast<std::size_t>(i)] = digits[h & 0xF];
    }
    return out;
}

// The user's file (public-lobbies-local.txt): for some rooms of the list, the name the user gave them and the password they typed (either may
// be empty). And the room in use when it left the list, as the list had it: "#kept <ListId of the list that left it out> address:port:name[:secret]"
// (what the user kept for it stays among the rooms above).
struct Local {
    std::vector<Room> rooms;
    bool has_kept = false;
    std::string kept_since;
    Room kept;
};

inline Local ParseLocal(const std::string& text) {
    Local local;
    for (const std::string& line : Lines(text)) {
        if (line.compare(0, 6, "#kept ") == 0) {
            const std::size_t space = line.find(' ', 6);
            Room room;
            if (space != std::string::npos && ParseRoomLine(line.substr(space + 1), room, true)) {
                local.has_kept = true;
                local.kept_since = line.substr(6, space - 6);
                local.kept = room;
            }
            continue;
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        Room room;
        if (ParseRoomLine(line, room, true) && (!room.name.empty() || !room.secret.empty())) {
            local.rooms.push_back(room);
        }
    }
    return local;
}

inline std::string RenderLocal(const Local& local) {
    std::string text = "# The names and passwords you gave the public rooms (3DS Link writes this file).\n";
    if (local.has_kept) {
        text += "#kept " + local.kept_since + " " + RoomLine(local.kept) + "\n";
    }
    for (const Room& r : local.rooms) {
        text += RoomLine(r) + "\n";
    }
    return text;
}

inline const Room* FindRoom(const std::vector<Room>& rooms, const std::string& host, std::uint16_t port) {
    for (const Room& r : rooms) {
        if (SameRoom(r.host, r.port, host, port)) {
            return &r;
        }
    }
    return nullptr;
}

// What the user keeps for a room: a name of their own ("" = the list's) and a password ("" = the list's, or none).
inline void SetLocal(Local& local, const Room& room, const std::string& name, const std::string& secret) {
    local.rooms.erase(std::remove_if(local.rooms.begin(), local.rooms.end(), [&](const Room& r) { return SameRoom(r, room); }), local.rooms.end());
    if (!name.empty() || !secret.empty()) {
        Room r = room;
        r.name = name;
        r.secret = secret;
        local.rooms.push_back(r);
    }
}

// A new list came. What the user keeps is cut to the rooms of the new list. The room in use (`active`, the room of AzaharLink.ini; `in_saved`:
// it is one of the user's saved rooms too, so the public list has no say on it) stays when it left the list, and only the first time: when
// the list changes again and it is still not there, `deselect` says that it is not the room in use any more.
struct Outcome {
    Local local;
    bool deselect = false;
};

inline Outcome Remember(const Local& old_local, const List& old_list, const List& fresh, const std::string& active_host, std::uint16_t active_port, bool in_saved) {
    Outcome out;
    const bool active_left = !active_host.empty() && !in_saved && !FindRoom(fresh.rooms, active_host, active_port);
    for (const Room& r : old_local.rooms) {
        if (FindRoom(fresh.rooms, r.host, r.port) || (active_left && SameRoom(r.host, r.port, active_host, active_port))) {
            out.local.rooms.push_back(r);
        }
    }
    if (!active_left) {
        return out; // (the kept room, if any, is not needed any more)
    }
    const std::string id = ListId(fresh);
    if (old_local.has_kept && SameRoom(old_local.kept.host, old_local.kept.port, active_host, active_port)) {
        if (old_local.kept_since == id) {
            out.local.has_kept = true;
            out.local.kept_since = id;
            out.local.kept = old_local.kept;
        } else {
            out.deselect = true;
            out.local.rooms.erase(std::remove_if(out.local.rooms.begin(), out.local.rooms.end(),
                                                 [&](const Room& r) { return SameRoom(r.host, r.port, active_host, active_port); }),
                                  out.local.rooms.end());
        }
        return out;
    }
    const Room* was = FindRoom(old_list.rooms, active_host, active_port);
    if (!was) {
        return out; // (the room in use was never a public one)
    }
    out.local.has_kept = true;
    out.local.kept_since = id;
    out.local.kept = *was;
    return out;
}

// One row of the page: a room of the list (or the kept one) with what the user keeps for it.
struct Entry {
    Room room;              // the name and the password shown and used: the user's when there are, else the list's
    std::string list_name;  // the name the list gives it
    std::string list_secret; // the password the list gives it ("" = none)
    bool own_name = false;  // the user renamed it
    bool kept = false;      // the room in use that left the list
};

inline Entry MakeEntry(const Room& r, const Local& local) {
    Entry e;
    e.room = r;
    e.list_name = r.name;
    e.list_secret = r.secret;
    if (const Room* mine = FindRoom(local.rooms, r.host, r.port)) {
        if (!mine->name.empty()) {
            e.room.name = mine->name;
            e.own_name = true;
        }
        if (!mine->secret.empty()) {
            e.room.secret = mine->secret;
        }
    }
    return e;
}

inline std::vector<Entry> Entries(const List& list, const Local& local) {
    std::vector<Entry> out;
    for (const Room& r : list.rooms) {
        out.push_back(MakeEntry(r, local));
    }
    if (local.has_kept && !FindRoom(list.rooms, local.kept.host, local.kept.port)) {
        Entry e = MakeEntry(local.kept, local);
        e.kept = true;
        out.push_back(e);
    }
    return out;
}

// A name a saved room can carry (settings: A-Z a-z 0-9 space . _ -, at most `max_chars`): the others become a space, a run of spaces one.
inline std::string SavedRoomName(const std::string& name, std::size_t max_chars) {
    std::string out;
    for (char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (ok) {
            out.push_back(c);
        } else if (!out.empty() && out.back() != ' ') {
            out.push_back(' ');
        }
        if (out.size() >= max_chars) {
            break;
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Titles: region and family
// ---------------------------------------------------------------------------------------------

enum class Region : std::uint8_t { Unknown, JPN, USA, EUR, KOR, TWN, Free };

inline const char* RegionName(Region r) {
    switch (r) {
    case Region::JPN:
        return "JPN";
    case Region::USA:
        return "USA";
    case Region::EUR:
        return "EUR";
    case Region::KOR:
        return "KOR";
    case Region::TWN:
        return "TWN";
    case Region::Free:
        return "Region free";
    default:
        return "?";
    }
}

struct Title {
    std::uint32_t id = 0; // the low 32 bits of the title id: an update or a DLC has those of its game
    Region region = Region::Unknown;
    char family[4] = {0, 0, 0, 0};
};

// The id a title is looked up with: a member that plays nothing has 0.
inline std::uint32_t TitleKey(std::uint64_t title_id) {
    return static_cast<std::uint32_t>(title_id & 0xFFFFFFFFull);
}

class Titles {
public:
    // "id:region:family" lines, ids in hex. False when not one line is a title (the old file is then kept).
    bool Parse(const std::string& text) {
        std::vector<Title> titles;
        for (const std::string& line : Lines(text)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            const std::size_t c1 = line.find(':');
            const std::size_t c2 = c1 == std::string::npos ? std::string::npos : line.find(':', c1 + 1);
            if (c2 == std::string::npos || c1 == 0 || c1 > 8 || line.size() - c2 - 1 != 3) {
                continue;
            }
            Title t;
            bool ok = true;
            for (std::size_t i = 0; i < c1 && ok; ++i) {
                const char c = line[i];
                const int digit = c >= '0' && c <= '9' ? c - '0' : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1));
                ok = digit >= 0;
                t.id = (t.id << 4) | static_cast<std::uint32_t>(digit < 0 ? 0 : digit);
            }
            const std::string region = line.substr(c1 + 1, c2 - c1 - 1);
            t.region = region == "JPN" ? Region::JPN
                       : region == "USA" ? Region::USA
                       : region == "EUR" ? Region::EUR
                       : region == "KOR" ? Region::KOR
                       : region == "TWN" ? Region::TWN
                       : region == "FREE" ? Region::Free
                                          : Region::Unknown;
            for (int i = 0; i < 3; ++i) {
                t.family[i] = line[c2 + 1 + static_cast<std::size_t>(i)];
            }
            if (ok && t.id != 0) {
                titles.push_back(t);
            }
        }
        if (titles.empty()) {
            return false;
        }
        std::sort(titles.begin(), titles.end(), [](const Title& a, const Title& b) { return a.id < b.id; });
        titles_.swap(titles);
        stamp_ = HeaderValue(text, "titles");
        return true;
    }

    const Title* Find(std::uint64_t title_id) const {
        const std::uint32_t key = TitleKey(title_id);
        if (key == 0) {
            return nullptr;
        }
        auto it = std::lower_bound(titles_.begin(), titles_.end(), key, [](const Title& t, std::uint32_t k) { return t.id < k; });
        return it != titles_.end() && it->id == key ? &*it : nullptr;
    }
    const std::string& stamp() const {
        return stamp_;
    }
    std::size_t size() const {
        return titles_.size();
    }
    void Clear() {
        std::vector<Title>().swap(titles_);
        stamp_.clear();
    }

private:
    std::vector<Title> titles_;
    std::string stamp_;
};

// ---------------------------------------------------------------------------------------------
// What a room said (lobby_probe.cpp fills it)
// ---------------------------------------------------------------------------------------------

struct Member {
    std::string nickname;
    std::string game;          // the name of what it plays ("" = nothing)
    std::uint64_t game_id = 0; // 0 = nothing
};

enum class State : std::uint8_t {
    Waiting,  // not asked yet
    Asking,   // asked, no answer yet
    Online,   // it answered (`ping`), and told its members when `has_info`
    Offline   // no connection at all
};

enum class Lock : std::uint8_t { Unknown, Open, Password };

struct Probe {
    State state = State::Waiting;
    int ping_ms = -1;
    bool has_info = false;   // it sent its name and members
    bool full = false;
    Lock lock = Lock::Unknown;
    bool secret_ok = false;  // we have a password and the room took it
    std::uint32_t version = 0; // its protocol, when an open room said it (0 = not known)
    std::string name;        // the name the room gives itself
    std::string description;
    std::uint32_t slots = 0;
    std::vector<Member> members;
};

inline int Playing(const Probe& p) {
    int n = 0;
    for (const Member& m : p.members) {
        n += m.game_id != 0 ? 1 : 0;
    }
    return n;
}

inline bool IsFull(const Probe& p) {
    return p.state == State::Online && (p.full || (p.has_info && p.slots > 0 && p.members.size() >= p.slots));
}

// ---------------------------------------------------------------------------------------------
// The order, the colours, the filters
// ---------------------------------------------------------------------------------------------

// Rooms with a free slot first (the lower ping first, then the fuller room), then the ones not asked yet, then the full ones, the offline ones
// last: every filter shows its rooms in this order.
inline int Group(const Probe& p) {
    if (p.state == State::Online) {
        return IsFull(p) ? 2 : 0;
    }
    return p.state == State::Offline ? 3 : 1;
}

inline bool Before(const Probe& a, const std::string& name_a, const Probe& b, const std::string& name_b) {
    const int ga = Group(a), gb = Group(b);
    if (ga != gb) {
        return ga < gb;
    }
    if (ga == 0 || ga == 2) {
        if (a.ping_ms != b.ping_ms) {
            return a.ping_ms < b.ping_ms;
        }
        if (a.members.size() != b.members.size()) {
            return a.members.size() > b.members.size();
        }
    }
    return Lower(name_a) < Lower(name_b);
}

enum class PingTier { Blue, Green, Yellow, Red };

inline PingTier TierOf(int ping_ms) {
    if (ping_ms <= 15) {
        return PingTier::Blue;
    }
    if (ping_ms <= 35) {
        return PingTier::Green;
    }
    return ping_ms <= 60 ? PingTier::Yellow : PingTier::Red;
}

// A title of the user: its id, and its family and region when titles.txt knows it.
struct Owned {
    std::uint32_t key = 0;
    const Title* title = nullptr;
};

// What a member plays against the user's titles: the same edition, the same game in the user's region (a twin version), or the same game of
// another region (the page says so: whether the two can play together depends on the game).
enum class Match { None, Same, Family, OtherRegion };

inline Match MatchOf(const Member& m, const std::vector<Owned>& owned, const Titles& titles, const Owned** which = nullptr) {
    const std::uint32_t key = TitleKey(m.game_id);
    if (key == 0) {
        return Match::None;
    }
    for (const Owned& o : owned) {
        if (o.key == key) {
            if (which) {
                *which = &o;
            }
            return Match::Same;
        }
    }
    const Title* t = titles.Find(m.game_id);
    if (!t) {
        return Match::None;
    }
    Match best = Match::None;
    for (const Owned& o : owned) {
        if (!o.title || std::string(o.title->family, 3) != std::string(t->family, 3)) {
            continue;
        }
        const bool same_region = o.title->region == t->region || o.title->region == Region::Free || t->region == Region::Free;
        if (same_region) {
            if (which) {
                *which = &o;
            }
            return Match::Family;
        }
        if (best == Match::None) {
            best = Match::OtherRegion;
            if (which) {
                *which = &o;
            }
        }
    }
    return best;
}

// The best match of a room's members: Same before Family before OtherRegion.
inline Match RoomMatch(const Probe& p, const std::vector<Owned>& owned, const Titles& titles) {
    Match best = Match::None;
    for (const Member& m : p.members) {
        const Match m_match = MatchOf(m, owned, titles);
        if (m_match == Match::Same) {
            return Match::Same;
        }
        if (m_match == Match::Family || (m_match == Match::OtherRegion && best == Match::None)) {
            best = m_match;
        }
    }
    return best;
}

enum class Kind { All, MyGames, FreeSlots };
constexpr int kKinds = 3;
inline const char* KindName(Kind k) {
    return k == Kind::MyGames ? "My games" : (k == Kind::FreeSlots ? "Free slots" : "All rooms");
}

// The region filter: All, then each region in turn.
constexpr Region kRegionFilters[] = {Region::Unknown, Region::JPN, Region::USA, Region::EUR, Region::KOR, Region::TWN, Region::Free};
constexpr int kRegionFilterCount = 7;
inline const char* RegionFilterName(Region r) {
    return r == Region::Unknown ? "All regions" : RegionName(r);
}

inline bool PassesKind(Kind kind, const Probe& p, const std::vector<Owned>& owned, const Titles& titles) {
    switch (kind) {
    case Kind::MyGames:
        return RoomMatch(p, owned, titles) != Match::None;
    case Kind::FreeSlots:
        return p.state == State::Online && !IsFull(p);
    default:
        return true;
    }
}

// A room passes a region when one of its players plays a title of that region, or when nobody in it plays anything (nothing says it is not).
inline bool PassesRegion(Region region, const Probe& p, const Titles& titles) {
    if (region == Region::Unknown || Playing(p) == 0) {
        return true;
    }
    for (const Member& m : p.members) {
        const Title* t = titles.Find(m.game_id);
        if (t && t->region == region) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// The search
// ---------------------------------------------------------------------------------------------

// The letters of a text, one number each (UTF-8 read into code points): Latin letters and digits in lower case, the letters of other alphabets as
// they are, nothing else. "MH4G / MH4U" -> "mh4gmh4u". A letter with an accent counts as one letter (a wrong one when it is typed without).
inline std::vector<std::uint32_t> SearchForm(const std::string& s) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = c;
        std::size_t n = 1;
        auto follows = [&](std::size_t k) {
            for (std::size_t j = 1; j < k; ++j) {
                if ((static_cast<unsigned char>(s[i + j]) & 0xC0) != 0x80) {
                    return false;
                }
            }
            return true;
        };
        if (c >= 0x80 && c < 0xC0) {
            ++i; // (a byte that continues nothing: dropped)
            continue;
        }
        if (c >= 0xF0 && i + 3 < s.size() && follows(4)) {
            cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu);
            n = 4;
        } else if (c >= 0xE0 && i + 2 < s.size() && follows(3)) {
            cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
            n = 3;
        } else if (c >= 0xC0 && i + 1 < s.size() && follows(2)) {
            cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu);
            n = 2;
        }
        i += n;
        if (n == 1 && c >= 0x80) {
            continue; // (a cut letter at the end, or a lead byte without its continuation)
        }
        if (cp < 0x80) {
            if (std::isalnum(static_cast<int>(cp))) {
                out.push_back(static_cast<std::uint32_t>(std::tolower(static_cast<int>(cp))));
            }
        } else if (cp >= 0xC0 && cp != 0xD7 && cp != 0xF7) {
            out.push_back(cp >= 0xC0 && cp <= 0xDE ? cp + 0x20 : cp); // (the accented Latin capitals of Latin-1 in lower case)
        }
    }
    return out;
}

// How many letters may be wrong: none up to 3, one up to 19, then one in ten.
inline std::size_t AllowedErrors(std::size_t letters) {
    if (letters <= 3) {
        return 0;
    }
    return letters < 20 ? 1 : letters / 10;
}

// The fewest changes (a letter wrong, missing or more) that make `query` a part of `text` (Sellers' algorithm).
inline std::size_t PartDistance(const std::vector<std::uint32_t>& query, const std::vector<std::uint32_t>& text) {
    std::vector<std::size_t> prev(query.size() + 1), cur(query.size() + 1);
    for (std::size_t i = 0; i <= query.size(); ++i) {
        prev[i] = i;
    }
    std::size_t best = query.size();
    for (std::size_t j = 1; j <= text.size(); ++j) {
        cur[0] = 0;
        for (std::size_t i = 1; i <= query.size(); ++i) {
            const std::size_t change = prev[i - 1] + (query[i - 1] == text[j - 1] ? 0 : 1);
            cur[i] = std::min(change, std::min(prev[i], cur[i - 1]) + 1);
        }
        best = std::min(best, cur[query.size()]);
        prev.swap(cur);
    }
    return best;
}

// The query in the room's names (the shown one, the list's, the room's own) or in the games played in it.
inline bool Matches(const std::string& query, const Entry& e, const Probe& p) {
    const std::vector<std::uint32_t> q = SearchForm(query);
    if (q.empty()) {
        return true;
    }
    const std::size_t allowed = AllowedErrors(q.size());
    std::vector<std::string> fields = {e.room.name, e.list_name, p.name};
    for (const Member& m : p.members) {
        fields.push_back(m.game);
    }
    for (const std::string& f : fields) {
        const std::vector<std::uint32_t> t = SearchForm(f);
        if (!t.empty() && PartDistance(q, t) <= allowed) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Quick join, and who is in a room
// ---------------------------------------------------------------------------------------------

// The room Quick join takes: it answered, it has a free slot, its password is not unknown, somebody plays one of the user's games (the same
// edition before the same game), and the lowest ping. -1 when there is none.
inline int QuickPick(const std::vector<Entry>& entries, const std::vector<Probe>& probes, const std::vector<Owned>& owned, const Titles& titles) {
    int best = -1;
    int best_rank = 99, best_ping = 0;
    for (std::size_t i = 0; i < entries.size() && i < probes.size(); ++i) {
        const Probe& p = probes[i];
        if (p.state != State::Online || IsFull(p) || (p.lock == Lock::Password && entries[i].room.secret.empty())) {
            continue;
        }
        const Match m = RoomMatch(p, owned, titles);
        const int rank = m == Match::Same ? 0 : (m == Match::Family ? 1 : 99);
        if (rank == 99) {
            continue;
        }
        if (best < 0 || rank < best_rank || (rank == best_rank && p.ping_ms < best_ping)) {
            best = static_cast<int>(i);
            best_rank = rank;
            best_ping = p.ping_ms;
        }
    }
    return best;
}

// The order the members are listed in: the biggest group of one game first (the user's game first on a tie), the others after it, the ones who
// play nothing last. Returns their indexes.
inline std::vector<std::size_t> MemberOrder(const Probe& p, const std::vector<Owned>& owned, const Titles& titles) {
    auto game_of = [&](const Member& m) {
        const Title* t = titles.Find(m.game_id);
        return t ? std::string(t->family, 3) : (m.game_id ? "#" + m.game : std::string());
    };
    std::vector<std::string> games;
    std::vector<int> counts;
    for (const Member& m : p.members) {
        const std::string g = game_of(m);
        if (g.empty()) {
            continue;
        }
        auto it = std::find(games.begin(), games.end(), g);
        if (it == games.end()) {
            games.push_back(g);
            counts.push_back(1);
        } else {
            ++counts[static_cast<std::size_t>(it - games.begin())];
        }
    }
    std::vector<std::size_t> order(p.members.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    auto rank = [&](std::size_t i) {
        const Member& m = p.members[i];
        const std::string g = game_of(m);
        if (g.empty()) {
            return std::make_pair(1, 0);
        }
        const int count = counts[static_cast<std::size_t>(std::find(games.begin(), games.end(), g) - games.begin())];
        const int mine = MatchOf(m, owned, titles) != Match::None ? 1 : 0;
        return std::make_pair(0, -(count * 2 + mine));
    };
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const auto ra = rank(a), rb = rank(b);
        if (ra != rb) {
            return ra < rb;
        }
        return game_of(p.members[a]) < game_of(p.members[b]);
    });
    return order;
}

} // namespace lobbies
