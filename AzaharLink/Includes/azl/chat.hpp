// In-game chat, the part that needs no console: the cleaning of the text, the lines kept for the overlay, and the on-screen touch
// keyboard as a state machine (where the keys are, what a tap does). Pure C++ (no 3ds.h, no CTRPF): Tests/chat_test.cpp runs it on a PC.
// The drawing and the touch reading are in Sources/ctr/chat_overlay.cpp (only built with -DAZL_CHAT_OVERLAY).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace azl::chat {

constexpr std::size_t kLines = 8;          ///< lines of incoming chat shown at once (a long message takes several)
constexpr std::size_t kLineChars = 60;     ///< characters of one shown line (the top screen is 400 px, the font 6 px: 66 fit; the rest is margin)
constexpr std::uint64_t kShowMs = 9000;    ///< how long a line stays on the screen
constexpr std::size_t kTypedChars = 100;   ///< what one can type (the room relays up to 500)

/// What may be drawn: printable ASCII only. Control characters and every byte of a multi-byte character are dropped (the overlay font is
/// ASCII; chat comes from other people's consoles and PCs: it is never trusted), spaces are collapsed, the result is trimmed and cut at `max`.
inline std::string Clean(const std::string& in, std::size_t max) {
    std::string out;
    bool space = true;
    for (unsigned char c : in) {
        if (c == ' ' || c == '\t') {
            if (!space) {
                out.push_back(' ');
                space = true;
            }
        } else if (c > 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
            space = false;
        }
        if (out.size() >= max) {
            break;
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

/// The last chat lines: the network thread pushes them, the overlay reads them.
class Log {
public:
    struct Line {
        std::string text;
        std::uint64_t at_ms = 0;
    };

    void Push(const std::string& nick, const std::string& message, std::uint64_t now_ms) {
        std::string n = Clean(nick, 12);
        std::string m = Clean(message, 200);
        if (m.empty()) {
            return;
        }
        const std::vector<std::string> parts = Wrap((n.empty() ? std::string("?") : n) + ": " + m);
        Lock();
        for (const std::string& part : parts) {
            lines_[next_ % kLines].text = part;
            lines_[next_ % kLines].at_ms = now_ms ? now_ms : 1;
            ++next_;
        }
        Unlock();
    }

    /// Splits a long message over several lines, at a space where there is one.
    static std::vector<std::string> Wrap(std::string line) {
        std::vector<std::string> parts;
        while (!line.empty() && parts.size() < kLines) {
            const std::size_t width = parts.empty() ? kLineChars : kLineChars - 2;
            std::size_t take = line.size();
            if (take > width) {
                take = width;
                const std::size_t space = line.rfind(' ', width);
                if (space != std::string::npos && space > width / 3) {
                    take = space;
                }
            }
            parts.push_back((parts.empty() ? "" : "  ") + line.substr(0, take));
            line.erase(0, take);
            while (!line.empty() && line[0] == ' ') {
                line.erase(0, 1);
            }
        }
        return parts;
    }

    /// The lines still to be shown, oldest first. Returns how many were written to `out` (room for kLines).
    std::size_t Visible(std::uint64_t now_ms, Line* out) const {
        std::size_t n = 0;
        Lock();
        const std::size_t count = next_ < kLines ? next_ : kLines;
        for (std::size_t i = 0; i < count; ++i) {
            const Line& l = lines_[(next_ - count + i) % kLines];
            if (l.at_ms != 0 && now_ms >= l.at_ms && now_ms - l.at_ms < kShowMs) {
                out[n++] = l;
            }
        }
        Unlock();
        return n;
    }

private:
    void Lock() const {
        while (flag_.test_and_set(std::memory_order_acquire)) {
        }
    }
    void Unlock() const {
        flag_.clear(std::memory_order_release);
    }
    mutable std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    Line lines_[kLines];
    std::size_t next_ = 0;
};

/// A key of the touch keyboard (bottom screen, 320 x 240).
struct Key {
    int x, y, w, h;
    char normal;        ///< what it types; 0 for the special keys
    char shifted;
    enum Kind { Char, Shift, Back, Space, Send, Close } kind;
    const char* label;
};

constexpr int kTextBarY = 96;   ///< the line that shows what is typed (the keys start at kKeysY)
constexpr int kKeysY = 120;
constexpr int kKeyH = 24;

/// The button that opens the keyboard: small, drawn while the keyboard is closed, in one of six places of the bottom screen (0 top-left, 1 top-middle,
/// 2 top-right, 3 bottom-left, 4 bottom-middle, 5 bottom-right: the same order as the chat lines), so it can sit where the game has nothing to touch.
constexpr int kButtonW = 32;
constexpr int kButtonH = 16;
constexpr int kDefaultButtonPosition = 2;
inline Key ChatButtonKey(int position) {
    const int col = (position >= 0 && position < 6) ? position % 3 : 2;
    const bool bottom = position >= 3 && position < 6;
    const int x = col == 0 ? 2 : (col == 1 ? (320 - kButtonW) / 2 : 320 - 2 - kButtonW);
    return Key{x, bottom ? 240 - 2 - kButtonH : 2, kButtonW, kButtonH, 0, 0, Key::Char, "Chat"};
}

inline const Key* Keys(std::size_t* count) {
    static const Key keys[] = {
        // row 0: digits (shifted: symbols)
        {0, kKeysY, 32, kKeyH, '1', '!', Key::Char, nullptr}, {32, kKeysY, 32, kKeyH, '2', '@', Key::Char, nullptr},
        {64, kKeysY, 32, kKeyH, '3', '#', Key::Char, nullptr}, {96, kKeysY, 32, kKeyH, '4', '$', Key::Char, nullptr},
        {128, kKeysY, 32, kKeyH, '5', '%', Key::Char, nullptr}, {160, kKeysY, 32, kKeyH, '6', '^', Key::Char, nullptr},
        {192, kKeysY, 32, kKeyH, '7', '&', Key::Char, nullptr}, {224, kKeysY, 32, kKeyH, '8', '*', Key::Char, nullptr},
        {256, kKeysY, 32, kKeyH, '9', '(', Key::Char, nullptr}, {288, kKeysY, 32, kKeyH, '0', ')', Key::Char, nullptr},
        // row 1
        {0, kKeysY + 24, 32, kKeyH, 'q', 'Q', Key::Char, nullptr}, {32, kKeysY + 24, 32, kKeyH, 'w', 'W', Key::Char, nullptr},
        {64, kKeysY + 24, 32, kKeyH, 'e', 'E', Key::Char, nullptr}, {96, kKeysY + 24, 32, kKeyH, 'r', 'R', Key::Char, nullptr},
        {128, kKeysY + 24, 32, kKeyH, 't', 'T', Key::Char, nullptr}, {160, kKeysY + 24, 32, kKeyH, 'y', 'Y', Key::Char, nullptr},
        {192, kKeysY + 24, 32, kKeyH, 'u', 'U', Key::Char, nullptr}, {224, kKeysY + 24, 32, kKeyH, 'i', 'I', Key::Char, nullptr},
        {256, kKeysY + 24, 32, kKeyH, 'o', 'O', Key::Char, nullptr}, {288, kKeysY + 24, 32, kKeyH, 'p', 'P', Key::Char, nullptr},
        // row 2 (nine keys, centred)
        {16, kKeysY + 48, 32, kKeyH, 'a', 'A', Key::Char, nullptr}, {48, kKeysY + 48, 32, kKeyH, 's', 'S', Key::Char, nullptr},
        {80, kKeysY + 48, 32, kKeyH, 'd', 'D', Key::Char, nullptr}, {112, kKeysY + 48, 32, kKeyH, 'f', 'F', Key::Char, nullptr},
        {144, kKeysY + 48, 32, kKeyH, 'g', 'G', Key::Char, nullptr}, {176, kKeysY + 48, 32, kKeyH, 'h', 'H', Key::Char, nullptr},
        {208, kKeysY + 48, 32, kKeyH, 'j', 'J', Key::Char, nullptr}, {240, kKeysY + 48, 32, kKeyH, 'k', 'K', Key::Char, nullptr},
        {272, kKeysY + 48, 32, kKeyH, 'l', 'L', Key::Char, nullptr},
        // row 3: shift, seven letters, backspace
        {0, kKeysY + 72, 48, kKeyH, 0, 0, Key::Shift, "Aa"}, {48, kKeysY + 72, 32, kKeyH, 'z', 'Z', Key::Char, nullptr},
        {80, kKeysY + 72, 32, kKeyH, 'x', 'X', Key::Char, nullptr}, {112, kKeysY + 72, 32, kKeyH, 'c', 'C', Key::Char, nullptr},
        {144, kKeysY + 72, 32, kKeyH, 'v', 'V', Key::Char, nullptr}, {176, kKeysY + 72, 32, kKeyH, 'b', 'B', Key::Char, nullptr},
        {208, kKeysY + 72, 32, kKeyH, 'n', 'N', Key::Char, nullptr}, {240, kKeysY + 72, 32, kKeyH, 'm', 'M', Key::Char, nullptr},
        {272, kKeysY + 72, 48, kKeyH, 0, 0, Key::Back, "<-"},
        // row 4: close, space, . ? and send
        {0, kKeysY + 96, 56, kKeyH, 0, 0, Key::Close, "Close"}, {56, kKeysY + 96, 160, kKeyH, ' ', ' ', Key::Space, "space"},
        {216, kKeysY + 96, 32, kKeyH, '.', ',', Key::Char, nullptr}, {248, kKeysY + 96, 32, kKeyH, '?', '!', Key::Char, nullptr},
        {280, kKeysY + 96, 40, kKeyH, 0, 0, Key::Send, "Send"},
    };
    if (count) {
        *count = sizeof(keys) / sizeof(keys[0]);
    }
    return keys;
}

/// The keyboard as a state machine: feed it the touch state once per frame. A tap acts on the press (not on the release); the
/// backspace repeats while held. Nothing here touches the game: the overlay draws what this says.
class Keyboard {
public:
    enum class Event { None, Opened, Closed, Send };

    Event OnTouch(bool down, int x, int y, std::uint64_t now_ms) {
        const bool pressed = down && !was_down_;
        const bool released = !down && was_down_;
        was_down_ = down;
        if (!down) {
            held_ = nullptr;
        }
        if (!open_) {
            if (pressed && Inside(button_, x, y)) {
                open_ = true;
                shift_ = false;
                held_ = &button_;
                return Event::Opened;
            }
            return Event::None;
        }
        if (pressed) {
            held_ = Find(x, y);
            repeat_at_ = now_ms + 450;
            return held_ ? Act(*held_) : Event::None;
        }
        if (down && held_ && held_->kind == Key::Back && now_ms >= repeat_at_) { // a held backspace erases on
            repeat_at_ = now_ms + 70;
            return Act(*held_);
        }
        (void)released;
        return Event::None;
    }

    bool is_open() const {
        return open_;
    }
    /// Where the button that opens the keyboard is (0..5, see ChatButtonKey).
    void SetButtonPosition(int position) {
        button_ = ChatButtonKey(position);
    }
    const Key& button() const {
        return button_;
    }
    bool shift() const {
        return shift_;
    }
    const std::string& text() const {
        return text_;
    }
    const Key* held() const {
        return held_;
    }
    /// The message to send (after Event::Send); the box is empty again and the keyboard stays open for the next line.
    std::string TakeMessage() {
        std::string m = Clean(sent_, kTypedChars);
        sent_.clear();
        return m;
    }
    /// What the text bar shows: the end of what is typed, `width` characters at most.
    std::string Tail(std::size_t width) const {
        return text_.size() <= width ? text_ : text_.substr(text_.size() - width);
    }
    void Close() {
        open_ = false;
        held_ = nullptr;
    }

private:
    static bool Inside(const Key& k, int x, int y) {
        return x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + k.h;
    }
    static const Key* Find(int x, int y) {
        std::size_t n = 0;
        const Key* keys = Keys(&n);
        for (std::size_t i = 0; i < n; ++i) {
            if (Inside(keys[i], x, y)) {
                return &keys[i];
            }
        }
        return nullptr;
    }
    Event Act(const Key& k) {
        switch (k.kind) {
        case Key::Char:
        case Key::Space:
            if (text_.size() < kTypedChars) {
                text_.push_back(shift_ ? k.shifted : k.normal);
            }
            if (k.kind == Key::Char && shift_) {
                shift_ = false; // Shift lasts for one character (a capital or a symbol), then it is off again
            }
            return Event::None;
        case Key::Shift:
            shift_ = !shift_;
            return Event::None;
        case Key::Back:
            if (!text_.empty()) {
                text_.pop_back();
            }
            return Event::None;
        case Key::Send:
            sent_ = text_;
            text_.clear();
            return Clean(sent_, kTypedChars).empty() ? Event::None : Event::Send;
        case Key::Close:
            open_ = false;
            return Event::Closed;
        }
        return Event::None;
    }

    bool open_ = false;
    bool shift_ = false;
    bool was_down_ = false;
    Key button_ = ChatButtonKey(kDefaultButtonPosition);
    const Key* held_ = nullptr;
    std::uint64_t repeat_at_ = 0;
    std::string text_;
    std::string sent_;
};

/// Takes the touch away from the game while the keyboard is open. The game reads the HID shared page itself, so the page is cleared; the
/// words that hold the touch are found by matching the position the framework reports (Learn), and only those are written.
class TouchScrubber {
public:
    static constexpr std::size_t kTouchFirst = 0xA8 / 4;
    static constexpr std::size_t kTouchEnd = 0x140 / 4;
    static constexpr std::size_t kPadFirst = 0x28 / 4;
    static constexpr std::size_t kPadEnd = 0xA8 / 4;
    static constexpr std::uint32_t kKeyTouch = 1u << 20;
    static constexpr std::size_t kMaxSlots = 24;

    /// While the finger is down: remember where its position and its key bit are.
    void Learn(const volatile std::uint32_t* page, unsigned x, unsigned y) {
        if (!page || (x == 0 && y == 0)) {
            return;
        }
        const std::uint32_t xy = (x & 0xFFFFu) | ((y & 0xFFFFu) << 16);
        for (std::size_t w = kTouchFirst; w < kTouchEnd; ++w) {
            if (page[w] == xy) {
                AddTouch(static_cast<std::uint16_t>(w));
            }
        }
        for (std::size_t w = kPadFirst; w < kPadEnd; ++w) {
            if ((page[w] & kKeyTouch) != 0 && (w - kPadFirst) % 4 < 3) { // (current, pressed, released of an entry)
                pad_seen_.store(true, std::memory_order_release);
            }
        }
    }

    /// Clears what was learned (the position words, the touch key in every pad entry). Safe to call from another thread, any time.
    void Scrub(volatile std::uint32_t* page) const {
        if (!page) {
            return;
        }
        const std::size_t n = n_touch_.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < n; ++i) {
            page[touch_[i]] = 0;
        }
        if (pad_seen_.load(std::memory_order_acquire)) {
            for (std::size_t w = kPadFirst; w < kPadEnd; ++w) {
                if ((w - kPadFirst) % 4 < 3) {
                    const std::uint32_t v = page[w];
                    if ((v & kKeyTouch) != 0) {
                        page[w] = v & ~kKeyTouch;
                    }
                }
            }
        }
    }

    /// While the touch is being hidden the framework's own touch reading sees the cleared page almost all the time. The scrubber therefore
    /// looks at the learned words BEFORE it clears them: a fresh non-zero position there means the HID service has just written a touch. Gives
    /// the position of the first learned word that holds one (x in the low half, y in the high half).
    bool Peek(const volatile std::uint32_t* page, std::uint32_t& xy) const {
        if (!page) {
            return false;
        }
        const std::size_t n = n_touch_.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t v = page[touch_[i]];
            if (v != 0) {
                xy = v;
                return true;
            }
        }
        return false;
    }

    std::size_t touch_slots() const {
        return n_touch_.load(std::memory_order_acquire);
    }
    bool pad_learned() const {
        return pad_seen_.load(std::memory_order_acquire);
    }

private:
    void AddTouch(std::uint16_t w) {
        const std::size_t n = n_touch_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < n; ++i) {
            if (touch_[i] == w) {
                return;
            }
        }
        if (n < kMaxSlots) {
            touch_[n] = w;
            n_touch_.store(n + 1, std::memory_order_release);
        }
    }
    std::atomic<std::size_t> n_touch_{0};
    std::atomic<bool> pad_seen_{false};
    std::uint16_t touch_[kMaxSlots] = {};
};

// ---------------------------------------------------------------------------------------------
// Room lists for the plugin menu: the saved lists, the public list, their pages (no console needed to test them)
// ---------------------------------------------------------------------------------------------

constexpr int kRoomLists = 10;             ///< servers.txt (list 1), servers2.txt ... servers10.txt
constexpr std::size_t kRoomsPerList = 50;  ///< rooms read from one list file (the app writes 10; a hand-made file may hold more)
constexpr std::size_t kRoomRows = 5;       ///< rooms on one page of the menu
constexpr std::uint16_t kDefaultRoomPort = 24872;
constexpr std::size_t kRoomNameChars = 100; ///< characters of a room name that are kept (the whole name of any lobby room; the old limit of 12 cut it)

struct RoomEntry {
    std::string name;     ///< may be empty
    std::string host;
    std::uint16_t port = kDefaultRoomPort;
    std::string password; ///< may be empty
};

/// The file of list `n` (1..kRoomLists): "servers.txt" for 1 (the file the app has always written), "servers<n>.txt" after.
inline std::string RoomListFile(int n) {
    return n <= 1 ? std::string("servers.txt") : "servers" + std::to_string(n) + ".txt";
}

/// One line of a list file: "host[:port]", "host[:port]|name" or "host[:port]|name|password" (the format of the app's servers.txt; the
/// password is everything after the second bar). A blank line, a comment (#) or a line whose host or port is not valid gives false.
inline bool ParseRoomLine(const std::string& raw, RoomEntry& out) {
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    std::size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
        ++start;
    }
    line.erase(0, start);
    if (line.empty() || line[0] == '#') {
        return false;
    }
    const std::size_t bar1 = line.find('|');
    std::string address = line.substr(0, bar1);
    RoomEntry e;
    if (bar1 != std::string::npos) {
        const std::size_t bar2 = line.find('|', bar1 + 1);
        e.name = Clean(line.substr(bar1 + 1, bar2 == std::string::npos ? std::string::npos : bar2 - bar1 - 1), kRoomNameChars);
        if (bar2 != std::string::npos) {
            e.password = line.substr(bar2 + 1);
            if (e.password.size() > 64) {
                e.password.resize(64);
            }
        }
    }
    std::string host = address;
    const std::size_t colon = address.rfind(':');
    if (colon != std::string::npos) {
        host = address.substr(0, colon);
        const std::string port_text = address.substr(colon + 1);
        if (port_text.empty() || port_text.size() > 5) {
            return false;
        }
        unsigned long port = 0;
        for (char c : port_text) {
            if (c < '0' || c > '9') {
                return false;
            }
            port = port * 10 + static_cast<unsigned long>(c - '0');
        }
        if (port == 0 || port > 65535) {
            return false;
        }
        e.port = static_cast<std::uint16_t>(port);
    }
    if (host.empty() || host.size() > 100) {
        return false;
    }
    for (unsigned char c : host) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    e.host = host;
    out = e;
    return true;
}

/// The rooms of a list file's text, in file order, at most `max`.
inline std::vector<RoomEntry> ParseRoomList(const std::string& text, std::size_t max = kRoomsPerList) {
    std::vector<RoomEntry> rooms;
    std::size_t pos = 0;
    while (pos < text.size() && rooms.size() < max) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        RoomEntry e;
        if (ParseRoomLine(text.substr(pos, end - pos), e)) {
            rooms.push_back(e);
        }
        pos = end + 1;
    }
    return rooms;
}

/// Puts the room `host`:`port` (the last one visited) first in `rooms`, keeping the order of the others. False when it is not in the list.
inline bool MoveRoomFirst(std::vector<RoomEntry>& rooms, const std::string& host, std::uint16_t port) {
    const auto same = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
            const char y = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
            if (x != y) {
                return false;
            }
        }
        return true;
    };
    for (std::size_t i = 0; i < rooms.size(); ++i) {
        if (rooms[i].port == port && same(rooms[i].host, host)) {
            const RoomEntry found = rooms[i];
            rooms.erase(rooms.begin() + static_cast<std::ptrdiff_t>(i));
            rooms.insert(rooms.begin(), found);
            return true;
        }
    }
    return false;
}

/// What a row shows: the name, else host:port (the port left out when it is the default).
inline std::string RoomLabel(const RoomEntry& e) {
    std::string text = e.host;
    if (e.port != kDefaultRoomPort) {
        text += ":" + std::to_string(e.port);
    }
    return Clean(e.name, kRoomNameChars).empty() ? text : Clean(e.name, kRoomNameChars) + "  " + text;
}

/// The public rooms the Setup app saves from the lobby (the same line format as a list: "host:port|name"; the password field is
/// kAskPassword when the room has one, so the menu asks for it). The plugin never fetches the lobby itself (no HTTP in the game's process).
inline std::string PublicRoomsFile() {
    return "public_rooms.txt";
}
constexpr const char* kAskPassword = "?";
constexpr std::size_t kPublicRooms = 300; ///< rooms read from the public list (the most lobby::Parse keeps)

/// Pages of `per` rooms for the plugin menu: how many pages `total` rooms make (at least 1) and the first room of page `page`.
inline std::size_t PageCount(std::size_t total, std::size_t per = kRoomRows) {
    return total == 0 ? 1 : (total + per - 1) / per;
}
inline std::size_t PageFirst(std::size_t page, std::size_t per = kRoomRows) {
    return page * per;
}

// ---------------------------------------------------------------------------------------------
// Where the chat lines are drawn on the top screen (a switch in the plugin menu)
// ---------------------------------------------------------------------------------------------

constexpr int kChatPositions = 6;   ///< 0 top-left, 1 top-middle, 2 top-right, 3 bottom-left, 4 bottom-middle, 5 bottom-right
constexpr int kTopWidth = 400;      ///< the top screen
constexpr int kTopHeight = 240;
constexpr int kCharWidth = 6;       ///< the framework's font
constexpr int kLineHeight = 12;
constexpr int kEdge = 4;

inline const char* ChatPositionName(int p) {
    static const char* const kNames[kChatPositions] = {"Top-left",    "Top-middle",    "Top-right",
                                                       "Bottom-left", "Bottom-middle", "Bottom-right"};
    return (p >= 0 && p < kChatPositions) ? kNames[p] : kNames[0];
}

/// The text of the position file: the place of the chat lines, then the place of the Chat button ("0 2"; digits 0..5, spaces between).
/// A missing or wrong digit is the default: lines top-left (where they always were), button top-right.
inline void ParseChatPositions(const std::string& text, int& lines, int& button) {
    lines = 0;
    button = kDefaultButtonPosition;
    int index = 0;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        if (c < '0' || c > '5' || index > 1) {
            break;
        }
        (index == 0 ? lines : button) = c - '0';
        ++index;
    }
}

/// Where line `index` of `count` shown lines (each `text_width` px wide) starts: the lines stack downwards from the top edge, or upwards
/// from the bottom edge (the newest stays lowest either way), and sit against the left edge, the middle or the right edge.
inline void ChatLineXY(int position, int text_width, int index, int count, int& x, int& y) {
    const int col = (position >= 0 && position < kChatPositions) ? position % 3 : 0;
    const bool bottom = position >= 3 && position < kChatPositions;
    x = col == 0 ? kEdge : (col == 1 ? (kTopWidth - text_width) / 2 : kTopWidth - kEdge - text_width);
    if (x < 0) {
        x = 0;
    }
    y = bottom ? kTopHeight - kEdge - (count - index) * kLineHeight : kEdge + index * kLineHeight;
}

} // namespace azl::chat
