// The in-game menu (see ctr/game_menu.hpp). Drawn with the framework's primitives on the game's own framebuffers while the game is
// paused (OSD::GetTopScreen / GetBottomScreen / SwapBuffers: "only when the process is paused"), read with its Controller. Nothing of
// it is allocated while it is closed. The password a room asks is typed on the plugin's own keyboard (azl/osk.hpp, drawn by
// azl/osk_draw.hpp on the bottom screen), the one the chat uses in the game: not the framework's.
#include <3ds.h>

#include <CTRPluginFramework.hpp>

#include <ctime>
#include <new>
#include <string>
#include <vector>

#include "azl/log.hpp"
#include "azl/osk.hpp"
#include "azl/osk_draw.hpp"
#include "azl/room_choices.hpp"
#include "ctr/game_menu.hpp"
#include "ctr/input_mirror.hpp"
#include "ctr/plugin.hpp"

namespace azl {
namespace ctr {

namespace {

using CTRPluginFramework::Color;
using CTRPluginFramework::Controller;
using CTRPluginFramework::Key;
using CTRPluginFramework::OSD;
using CTRPluginFramework::Process;
using CTRPluginFramework::Screen;
using CTRPluginFramework::Touch;

// The keyboard reads the pad's bits as the HID reports them (azl/osk.hpp has no framework): the framework's Key are the same bits.
static_assert(osk::kKeyA == static_cast<u32>(Key::A) && osk::kKeyB == static_cast<u32>(Key::B) && osk::kKeySelect == static_cast<u32>(Key::Select) &&
                  osk::kKeyStart == static_cast<u32>(Key::Start) && osk::kKeyL == static_cast<u32>(Key::L) && osk::kKeyR == static_cast<u32>(Key::R),
              "the keyboard's buttons are the framework's");
static_assert(osk::kKeyRight == static_cast<u32>(Key::DPadRight) && osk::kKeyLeft == static_cast<u32>(Key::DPadLeft) &&
                  osk::kKeyUp == static_cast<u32>(Key::DPadUp) && osk::kKeyDown == static_cast<u32>(Key::DPadDown),
              "the keyboard's D-pad is the framework's");
static_assert(osk::kCpadRight == static_cast<u32>(Key::CPadRight) && osk::kCpadLeft == static_cast<u32>(Key::CPadLeft) &&
                  osk::kCpadUp == static_cast<u32>(Key::CPadUp) && osk::kCpadDown == static_cast<u32>(Key::CPadDown) &&
                  osk::kKeyTouch == static_cast<u32>(Key::Touchpad),
              "the keyboard's Circle Pad and touch are the framework's");

// The colours of 3DS Link (ConfigApp/source/gfx.hpp).
const Color kBg(13, 14, 17);
const Color kPanel(24, 26, 31);
const Color kPanelAlt(30, 33, 39);
const Color kLine(46, 50, 58);
const Color kBar(222, 225, 231);
const Color kBarText(20, 22, 27);
const Color kText(232, 234, 238);
const Color kDim(138, 144, 156);
const Color kAccent(70, 178, 255);
const Color kSel(34, 56, 90);
const Color kOn(74, 218, 128);
const Color kOff(244, 100, 100);
const Color kWarn(255, 198, 88);
const Color kBlue(86, 150, 255);

constexpr int kTopW = 400, kBottomW = 320, kScreenH = 240;
constexpr int kLineH = 16;     // the system font's line
constexpr int kRowH = 22;      // a row of the menu
constexpr u64 kInfoMs = 500;   // the room's information is copied at most this often (it holds the member list)
constexpr const char* kServersPath = "/3ds/3DSLink/servers.txt";
constexpr const char* kLobbiesPath = "/3ds/3DSLink/public-lobbies.txt";
constexpr const char* kLobbiesLocalPath = "/3ds/3DSLink/public-lobbies-local.txt";
constexpr const char* kPingsPath = "/3ds/3DSLink/public-lobbies-ping.txt"; // (written by the app: ConfigApp/source/pages_lobbies.cpp, SavePings)

// ---------------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------------

int TextW(const std::string& s) {
    return static_cast<int>(OSD::GetTextWidth(true, s));
}

// `s` cut with "..." to `max_w` pixels (never in the middle of a UTF-8 letter).
std::string Fit(const std::string& s, int max_w) {
    if (TextW(s) <= max_w) {
        return s;
    }
    std::string cut = s;
    while (!cut.empty() && TextW(cut + "...") > max_w) {
        cut.pop_back();
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) {
            cut.pop_back();
        }
        if (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0xC0) {
            cut.pop_back(); // (the first byte of the letter whose rest just went)
        }
    }
    return cut + "...";
}

// At most `max_lines` lines of `max_w` pixels, on spaces; the last one ends with "..." when the text goes on.
std::vector<std::string> Wrap(const std::string& s, int max_w, int max_lines) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t pos = 0;
    while (pos <= s.size()) {
        std::size_t space = s.find(' ', pos);
        if (space == std::string::npos) {
            space = s.size();
        }
        const std::string word = s.substr(pos, space - pos);
        const std::string tried = line.empty() ? word : line + " " + word;
        if (!line.empty() && TextW(tried) > max_w) {
            lines.push_back(line);
            line = word;
            if (static_cast<int>(lines.size()) == max_lines) {
                lines.back() = Fit(lines.back() + " " + s.substr(pos), max_w);
                return lines;
            }
        } else {
            line = tried;
        }
        pos = space + 1;
    }
    if (!line.empty() || lines.empty()) {
        lines.push_back(Fit(line, max_w));
    }
    return lines;
}

void Text(const Screen& scr, int x, int y, const Color& c, const std::string& s) {
    scr.DrawSysfont(s, static_cast<u32>(x), static_cast<u32>(y), c);
}

void TextRight(const Screen& scr, int right, int y, const Color& c, const std::string& s) {
    Text(scr, right - TextW(s), y, c, s);
}

void Rect(const Screen& scr, int x, int y, int w, int h, const Color& c) {
    if (w > 0 && h > 0) {
        scr.DrawRect(static_cast<u32>(x), static_cast<u32>(y), static_cast<u32>(w), static_cast<u32>(h), c, true);
    }
}

// The bar of a screen, as in the app: light, the accent at its left edge, the title in dark letters.
void Bar(const Screen& scr, int w, const std::string& title, const std::string& right) {
    Rect(scr, 0, 0, w, 24, kBar);
    Rect(scr, 0, 0, 5, 24, kAccent);
    Rect(scr, 0, 24, w, 1, kAccent);
    Text(scr, 14, 4, kBarText, title);
    if (!right.empty()) {
        TextRight(scr, w - 10, 4, Color(110, 114, 124), right);
    }
}

// A switch as in the app's Settings: the track (green when on) with its knob, and ON / OFF before it.
void Switch(const Screen& scr, int right, int y, bool on) {
    const int w = 34, h = 14, x = right - w;
    const Color track = on ? kOn : Color(74, 80, 92);
    Rect(scr, x, y + 3, w, h, track);
    Rect(scr, on ? x + w - h + 2 : x + 2, y + 5, h - 4, h - 4, Color(240, 242, 246));
    TextRight(scr, x - 8, y + 2, on ? kOn : kOff, on ? "ON" : "OFF");
}

// A key of the pad as in the app's footer: a light chip with the key, the word after it. Returns where the next one may start.
int KeyHint(const Screen& scr, int x, int y, const char* key, const std::string& label) {
    const int chip = TextW(key) + 10;
    Rect(scr, x, y, chip, 18, kBar);
    Text(scr, x + 5, y + 1, kBarText, key);
    Text(scr, x + chip + 6, y + 1, kText, label);
    return x + chip + 6 + TextW(label) + 14;
}

Color PingColor(u32 ms) {
    return ms <= 15 ? kBlue : ms <= 35 ? kOn : ms <= 60 ? kWarn : kOff;
}

// ---------------------------------------------------------------------------------------------
// The menu
// ---------------------------------------------------------------------------------------------

enum class View { Main, Choose, Saved, Public };
enum Row { kChange = 0, kLeaveJoin, kChat, kChatPos, kDebug, kTurnOn };

struct Menu {
    bool active = true;
    View view = View::Main;
    int sel = 0;
    int first = 0;                 // the first row of a list on the screen
    rooms::RoomFile file;          // the list shown (Saved and Public only)
    std::size_t saved_count = 0, public_count = 0; // (Choose)
    // The room, copied every kInfoMs.
    u64 info_ms = 0;
    RoomClient::State state = RoomClient::State::Idle;
    RoomClient::Error error = RoomClient::Error::None;
    u32 rtt_ms = 0;
    room::RoomInfo info;
    std::string status;
    // The room said "wrong password" and the keyboard was already shown for that refusal (the next attempt that is refused asks again).
    bool refusal_asked = false;
    rooms::SortBy sort = rooms::SortBy::Ping; // (the public lobbies; L and R switch it)
    u32 repeat_key = 0;
    u64 repeat_ms = 0;
    // The keyboard of the password while it is open (made when the room refuses, deleted when it closes), and what it drew last: the
    // bottom screen is drawn again only when that changes, twice (the framework's two buffers take turns).
    osk::Osk* osk = nullptr;
    osk::View osk_drawn = {};
    int osk_redraw = 0;
};

std::string g_picked_name; // (the name of the last room picked in the menu, while the room has not said its own: kept between openings)

std::vector<int> MainRows(const Menu& m) {
    if (!m.active) {
        return {kTurnOn};
    }
    std::vector<int> rows = {kChange};
    if (!Plugin::Get().config().host.empty()) {
        rows.push_back(kLeaveJoin);
    }
    rows.push_back(kChat);
    rows.push_back(kChatPos);
    rows.push_back(kDebug);
    return rows;
}

int RowCount(const Menu& m) {
    switch (m.view) {
    case View::Main:
        return static_cast<int>(MainRows(m).size());
    case View::Choose:
        return 2;
    default:
        return m.file.size() == 0 ? 1 : static_cast<int>(m.file.size());
    }
}

void Refresh(Menu& m, bool force) {
    const u64 now = osGetTime();
    if (!force && now - m.info_ms < kInfoMs) {
        return;
    }
    m.info_ms = now;
    Plugin& plugin = Plugin::Get();
    if (!m.active) {
        return;
    }
    RoomClient& room = plugin.room();
    m.state = room.state();
    m.error = room.error();
    m.rtt_ms = room.link_stats().rtt_ms;
    if (m.state == RoomClient::State::Joined) {
        m.info = room.room_info();
    } else {
        m.info = room::RoomInfo();
    }
    m.status = plugin.StatusLine();
}

// What the room 3DS Link is in says this moment: its ping (ENet's, of the link the plugin keeps anyway: nothing more is asked) and its
// players and slots.
rooms::Seen LiveSeen(const Menu& m) {
    rooms::Seen live;
    live.ping = static_cast<std::int16_t>(m.rtt_ms > 9999 ? 9999 : m.rtt_ms);
    live.players = static_cast<std::uint8_t>(m.info.members.size() > 255 ? 255 : m.info.members.size());
    live.slots = static_cast<std::uint8_t>(m.info.member_slots > 255 ? 255 : m.info.member_slots);
    return live;
}

void LoadList(Menu& m, View view) {
    std::string text, local;
    if (view == View::Saved) {
        Plugin::ReadDataFile(kServersPath, rooms::kMaxListBytes, text);
        m.file.Load(rooms::RoomFile::Kind::Saved, std::move(text));
    } else {
        Plugin::ReadDataFile(kLobbiesPath, rooms::kMaxListBytes, text);
        Plugin::ReadDataFile(kLobbiesLocalPath, rooms::kMaxLocalBytes, local);
        m.file.Load(rooms::RoomFile::Kind::Public, std::move(text), std::move(local));
    }
    // What the app measured the last time its page Public lobbies asked the rooms (the plugin never asks a room itself): read, kept as 4
    // bytes a room, and the text let go.
    std::string pings;
    Plugin::ReadDataFile(kPingsPath, rooms::kMaxPingBytes, pings);
    m.file.ApplyPings(pings);
    if (m.state == RoomClient::State::Joined) {
        // (the room 3DS Link is in takes its place in the order with what it says now)
        const Config& cfg = Plugin::Get().config();
        m.file.SetSeen(m.file.Find(cfg.host, cfg.port), LiveSeen(m));
    }
    if (view == View::Public) {
        m.file.Sort(m.sort);
    }
    m.view = view;
    m.sel = 0;
    m.first = 0;
}

void OpenChoose(Menu& m) {
    LoadList(m, View::Saved);
    m.saved_count = m.file.size();
    LoadList(m, View::Public);
    m.public_count = m.file.size();
    m.file.Clear();
    m.view = View::Choose;
    m.sel = 0;
}

// "Be in the room" off for one round of the plugin's work, then on: the plugin leaves the room it is in, forgets the last error, and
// joins the room of its settings (the way the old menu's switch started over).
void Rejoin(const GameMenuHooks& hooks) {
    *hooks.want_connected = false;
    hooks.tick();
    *hooks.want_connected = true;
    hooks.tick();
}

void Pick(Menu& m, const rooms::Choice& c, const GameMenuHooks& hooks) {
    Plugin& plugin = Plugin::Get();
    Config& cfg = plugin.config();
    cfg.host = c.host;
    cfg.port = c.port;
    cfg.password = c.password;
    plugin.SaveConfig();
    g_picked_name = c.name;
    Log("menu: room changed to %s:%u ('%s', %s)", c.host.c_str(), c.port, c.name.c_str(), c.password.empty() ? "no password" : "a password");
    plugin.TakeNotifications(); // (what the old room said is old news: the game hears of the new one)
    Rejoin(hooks);
    m.file.Clear();
    m.view = View::Main;
    m.sel = 0;
    Refresh(m, true);
}

// The room said the password is wrong (a room picked here, or Join with the password kept): the keyboard opens on the bottom screen
// with the password kept (masked), and OK joins the room again with what it says. Every refusal asks again; Cancel (or SELECT, or B with
// nothing typed) leaves it until the next attempt.
void AskPassword(Menu& m) {
    m.refusal_asked = true;
    osk::Options o;
    o.max_len = 64;
    o.mask = true;
    o.title = "Room password";
    o.submit = "OK";
    m.osk = new (std::nothrow) osk::Osk(o, Plugin::Get().config().password.c_str());
    if (!m.osk) {
        Log("menu: no memory for the keyboard: the room's password is not asked");
        return;
    }
    m.osk_redraw = 2;
}

// One round of the open keyboard: the pad and the touch screen go to it, not to the menu (its own edges from the keys held: the
// framework's "pressed" would do here, the game being paused, but the keyboard is the code of the running game too, where it would not).
void FeedKeyboard(Menu& m, const GameMenuHooks& hooks) {
    Controller::Update();
    osk::Input in;
    in.held = Controller::GetKeysDown();
    in.touch = (in.held & Key::Touchpad) != 0;
    if (in.touch) {
        const CTRPluginFramework::UIntVector p = Touch::GetPosition();
        in.tx = static_cast<u16>(p.x);
        in.ty = static_cast<u16>(p.y);
    }
    in.now_ms = static_cast<u32>(osGetTime());
    const osk::Result r = m.osk->Feed(in);
    if (r == osk::Result::kNone) {
        return;
    }
    if (r == osk::Result::kSubmit) {
        Config& cfg = Plugin::Get().config();
        cfg.password.assign(m.osk->text(), m.osk->length());
        Plugin::Get().SaveConfig();
        Log("menu: room password typed after a refusal");
    }
    delete m.osk;
    m.osk = nullptr;
    m.repeat_key = 0; // (a direction of the menu held from before the keyboard opened does not repeat on the menu when it closes)
    if (r == osk::Result::kSubmit) {
        Rejoin(hooks);
        Refresh(m, true);
    }
}

// The framework's screen as the plugin's drawing takes it (azl/gfx_surface.hpp): the right image only on the top screen with the 3D on.
gfx::Surface SurfaceOf(const Screen& scr) {
    return gfx::ScreenSurface(reinterpret_cast<u8*>(scr.LeftFramebuffer),
                              scr.IsTop && scr.Is3DEnabled ? reinterpret_cast<u8*>(scr.RightFramebuffer) : nullptr, scr.Stride,
                              static_cast<u32>(scr.Format), static_cast<u16>(scr.IsTop ? kTopW : kBottomW), static_cast<u16>(kScreenH));
}

// The keyboard on the bottom screen, when what it shows changed (and once more, for the other buffer).
void DrawKeyboard(Menu& m, const Screen& scr) {
    osk::View v;
    m.osk->Snapshot(&v);
    if (!osk::Same(v, m.osk_drawn)) {
        m.osk_drawn = v;
        m.osk_redraw = 2;
    }
    if (m.osk_redraw > 0) {
        --m.osk_redraw;
        osk::DrawOsk(SurfaceOf(scr), v);
    }
}

// A pressed key: false when the menu closes.
bool Handle(Menu& m, u32 down, const GameMenuHooks& hooks) {
    const int count = RowCount(m);
    const int on_row = m.view == View::Main && m.sel < count ? MainRows(m)[static_cast<std::size_t>(m.sel)] : -1;
    if ((on_row == kDebug || on_row == kChat || on_row == kChatPos) && (down & (Key::DPadLeft | Key::DPadRight))) {
        // (the switches as in the Settings of the app: right is on, left is off; the corner goes round the four, right forwards)
        Plugin& plugin = Plugin::Get();
        const bool right = (down & Key::DPadRight) != 0;
        if (on_row == kDebug) {
            const int level = right ? 1 : 0;
            if (plugin.debug_level() != level) {
                plugin.SetDebugLevel(level);
            }
        } else if (on_row == kChat) {
            if (plugin.chat_on() != right) {
                plugin.SetChatOn(right);
            }
        } else {
            plugin.SetChatPos(ini::NextChatPos(plugin.chat_pos(), right ? 1 : -1));
        }
    } else if (down & Key::DPadUp) {
        m.sel = m.sel > 0 ? m.sel - 1 : count - 1;
    } else if (down & Key::DPadDown) {
        m.sel = m.sel + 1 < count ? m.sel + 1 : 0;
    } else if (down & Key::B) {
        if (m.view == View::Main) {
            return false;
        }
        const bool from_list = m.view == View::Saved || m.view == View::Public;
        m.file.Clear();
        if (from_list) {
            const int back_to = m.view == View::Public ? 1 : 0;
            OpenChoose(m);
            m.sel = back_to;
        } else {
            m.view = View::Main;
            m.sel = 0;
        }
    } else if ((down & (Key::L | Key::R)) && m.view == View::Public && m.file.size() > 1) {
        m.sort = m.sort == rooms::SortBy::Ping ? rooms::SortBy::Name : rooms::SortBy::Ping;
        m.file.Sort(m.sort);
        m.sel = 0;
        m.first = 0;
    } else if (down & Key::A) {
        Plugin& plugin = Plugin::Get();
        if (m.view == View::Main) {
            const int row = MainRows(m)[static_cast<std::size_t>(m.sel)];
            if (row == kChange) {
                OpenChoose(m);
            } else if (row == kLeaveJoin) {
                *hooks.want_connected = !*hooks.want_connected;
                Log("menu: %s the room", *hooks.want_connected ? "join" : "leave");
                hooks.tick();
                if (!*hooks.want_connected) {
                    // (what the room said while the menu was open - "Joined room ..." - is old news now: the game hears that it left)
                    plugin.TakeNotifications();
                    plugin.PostNotification("Left the room: 3DS Link is in no room now");
                }
                Refresh(m, true);
            } else if (row == kChat) {
                plugin.SetChatOn(!plugin.chat_on());
            } else if (row == kChatPos) {
                plugin.SetChatPos(ini::NextChatPos(plugin.chat_pos(), 1));
            } else if (row == kDebug) {
                plugin.SetDebugLevel(plugin.debug_level() == 0 ? 1 : 0);
            } else if (row == kTurnOn) {
                Config& cfg = plugin.config();
                cfg.enabled = true;
                cfg.tunnel_enabled = true;
                plugin.SaveConfig();
                OSD::Notify("AzaharLink will be ON at the next game start");
            }
        } else if (m.view == View::Choose) {
            LoadList(m, m.sel == 0 ? View::Saved : View::Public);
        } else {
            rooms::Choice c;
            if (m.file.Get(static_cast<std::size_t>(m.sel), c)) {
                Pick(m, c, hooks);
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------

std::string RoomTitle(const Menu& m) {
    if (!m.info.name.empty()) {
        return m.info.name;
    }
    const Config& cfg = Plugin::Get().config();
    if (cfg.host.empty()) {
        return "No room chosen yet";
    }
    return g_picked_name.empty() ? cfg.host + ":" + std::to_string(cfg.port) : g_picked_name;
}

// What is known of room `i` of the list: what the app measured, or - the room 3DS Link is in now - what the room says this moment.
rooms::Seen RowSeen(const Menu& m, std::size_t i, const rooms::Choice& c) {
    const Config& cfg = Plugin::Get().config();
    if (m.state == RoomClient::State::Joined && c.port == cfg.port && rooms::SameHost(c.host, cfg.host)) {
        return LiveSeen(m);
    }
    return m.file.seen(i);
}

std::string PlayersText(const rooms::Seen& s) {
    if (s.ping < 0) {
        return std::string();
    }
    return std::to_string(s.players) + (s.slots > 0 ? "/" + std::to_string(s.slots) : std::string());
}

// How long ago the app measured the pings ("" without a file).
std::string Age(std::uint64_t measured) {
    if (measured == 0) {
        return std::string();
    }
    const std::uint64_t now = static_cast<std::uint64_t>(std::time(nullptr));
    const std::uint64_t s = now > measured ? now - measured : 0;
    if (s < 120) {
        return "just now";
    }
    if (s < 2 * 3600) {
        return std::to_string(s / 60) + " min ago";
    }
    if (s < 2 * 86400) {
        return std::to_string(s / 3600) + " h ago";
    }
    return std::to_string(s / 86400) + " days ago";
}

// A room of a list: its name, then the players and the ping the app measured (the ping in the colours of the app; "offline" in red).
void DrawRoomRow(const Screen& scr, int y, bool selected, const std::string& label, const rooms::Seen& seen) {
    Rect(scr, 8, y, kTopW - 16, kRowH - 2, selected ? kSel : kPanel);
    if (selected) {
        Rect(scr, 8, y, 3, kRowH - 2, kAccent);
    }
    int right = kTopW - 18;
    if (seen.ping == -1) {
        TextRight(scr, right, y + 2, kOff, "offline");
        right -= TextW("offline") + 12;
    } else if (seen.ping >= 0) {
        const std::string ping = std::to_string(seen.ping) + " ms";
        TextRight(scr, right, y + 2, PingColor(static_cast<u32>(seen.ping)), ping);
        right -= 62; // (a column: the players line up whatever the ping)
        const std::string players = PlayersText(seen);
        const bool full = seen.slots > 0 && seen.players >= seen.slots;
        TextRight(scr, right, y + 2, full ? kOff : kDim, players);
        right -= 52;
    }
    Text(scr, 20, y + 2, kText, Fit(label, right - 20));
}

void DrawRow(const Screen& scr, int y, bool selected, const std::string& label, const std::string& value, const Color& value_color, bool link,
             const Color& label_color = kText) {
    Rect(scr, 8, y, kTopW - 16, kRowH - 2, selected ? kSel : kPanel);
    if (selected) {
        Rect(scr, 8, y, 3, kRowH - 2, kAccent);
    }
    int right = kTopW - 18;
    if (link) {
        Text(scr, right - 6, y + 2, kDim, ">");
        right -= 16;
    }
    if (!value.empty()) {
        TextRight(scr, right, y + 2, value_color, value);
        right -= TextW(value) + 10;
    }
    Text(scr, 20, y + 2, label_color, Fit(label, right - 20));
}

void DrawTop(const Menu& m, const Screen& scr) {
    Rect(scr, 0, 0, kTopW, kScreenH, kBg);
    Bar(scr, kTopW, m.osk ? "The room wants a password" : "3DS Link  -  In-game menu", std::string("v") + kVersion);

    // The room 3DS Link is in: its state (and ping), its name on two lines.
    Rect(scr, 8, 30, kTopW - 16, 64, kPanel);
    Plugin& plugin = Plugin::Get();
    if (!m.active) {
        Rect(scr, 8, 30, 3, 64, kOff);
        Text(scr, 20, 34, kOff, "AzaharLink is OFF in this game");
        const std::string why = plugin.startup_issue().empty() ? std::string("It is switched off in the settings of 3DS Link.") : plugin.startup_issue();
        int y = 54;
        for (const std::string& line : Wrap(why, kTopW - 40, 2)) {
            Text(scr, 20, y, kDim, line);
            y += kLineH;
        }
    } else {
        const bool joined = m.state == RoomClient::State::Joined;
        const bool trying = m.state == RoomClient::State::Connecting || m.state == RoomClient::State::Joining ||
                            (m.state == RoomClient::State::Idle && m.status.compare(0, 4, "Reco") == 0) ||
                            (m.state == RoomClient::State::Idle && m.status.compare(0, 4, "Wait") == 0);
        const Color stripe = joined ? kOn : trying ? kWarn : kDim;
        Rect(scr, 8, 30, 3, 64, stripe);
        Rect(scr, 20, 38, 8, 8, stripe);
        std::string state = joined ? "Connected" : m.status;
        int x = 34;
        if (joined) {
            Text(scr, x, 34, kText, state);
            x += TextW(state) + 12;
            if (m.rtt_ms > 0) {
                const std::string ping = std::to_string(m.rtt_ms) + " ms";
                Text(scr, x, 34, PingColor(m.rtt_ms), ping);
            }
        } else {
            Text(scr, x, 34, trying ? kWarn : kDim, Fit(state, kTopW - 60));
        }
        int y = 54;
        for (const std::string& line : Wrap(RoomTitle(m), kTopW - 40, 2)) {
            Text(scr, 20, y, kText, line);
            y += kLineH;
        }
    }

    // The menu.
    const int list_y = 102;
    const int visible = 5;
    if (m.view == View::Main) {
        const std::vector<int> rows = MainRows(m);
        int y = list_y - kRowH;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const bool sel = static_cast<int>(i) == m.sel;
            // (the chat and the debug log are groups of their own: a gap above each. With the room's two rows the last one ends at
            // 102 + 4 * 22 + 2 * 8 + 20 = 226, inside the 240 of the screen)
            y += kRowH + (rows[i] == kChat || rows[i] == kDebug ? 8 : 0);
            switch (rows[i]) {
            case kChange:
                DrawRow(scr, y, sel, "Change room", "", kDim, true);
                break;
            case kLeaveJoin:
                DrawRow(scr, y, sel, m.active && m.state != RoomClient::State::Idle ? "Leave the room" : "Join the room", "", kDim, false);
                break;
            case kChat:
                DrawRow(scr, y, sel, "In-game chat", "", kDim, false);
                Switch(scr, kTopW - 18, y, plugin.chat_on());
                break;
            case kChatPos: {
                // (dim while the chat is off: the corner is kept for when it is on again)
                const bool chat_on = plugin.chat_on();
                DrawRow(scr, y, sel, "Chat position", std::string("< ") + ini::ChatPosLabel(plugin.chat_pos()) + " >", chat_on ? kText : kDim, false,
                        chat_on ? kText : kDim);
                break;
            }
            case kDebug:
                DrawRow(scr, y, sel, "Debug log", "", kDim, false);
                Switch(scr, kTopW - 18, y, plugin.debug_level() > 0);
                break;
            case kTurnOn:
                DrawRow(scr, y, sel, "Switch it on for the next start", "", kDim, false);
                break;
            }
        }
    } else if (m.view == View::Choose) {
        DrawRow(scr, list_y, m.sel == 0, "Favourites", std::to_string(m.saved_count) + (m.saved_count == 1 ? " room" : " rooms"), kDim, true);
        DrawRow(scr, list_y + kRowH, m.sel == 1, "Public lobbies", std::to_string(m.public_count) + (m.public_count == 1 ? " room" : " rooms"),
                kDim, true);
    } else {
        Text(scr, 14, list_y - 2, kAccent, m.view == View::Saved ? "Favourites" : "Public lobbies");
        if (m.file.size() == 0) {
            DrawRow(scr, list_y + 18, true,
                    m.view == View::Saved ? "No saved room: add one in 3DS Link" : "No list yet: open Public lobbies in 3DS Link", "", kDim, false);
        } else {
            const int first = m.first;
            for (int i = 0; i < visible && first + i < static_cast<int>(m.file.size()); ++i) {
                rooms::Choice c;
                m.file.Get(static_cast<std::size_t>(first + i), c);
                const std::string label = c.name.empty() ? c.host + ":" + std::to_string(c.port) : c.name;
                DrawRoomRow(scr, list_y + 18 + i * kRowH, first + i == m.sel, label, RowSeen(m, static_cast<std::size_t>(first + i), c));
            }
            std::string where = std::to_string(m.sel + 1) + " / " + std::to_string(m.file.size());
            if (m.view == View::Public) {
                where = std::string(m.sort == rooms::SortBy::Ping ? "by ping" : "by name") + "   " + where;
            }
            TextRight(scr, kTopW - 14, list_y - 2, kDim, where);
        }
    }
}

void DrawBottom(const Menu& m, const Screen& scr) {
    Rect(scr, 0, 0, kBottomW, kScreenH, kBg);
    const bool list = m.view == View::Saved || m.view == View::Public;
    Bar(scr, kBottomW, list ? "The room under the cursor" : "The room", "");
    int y = 32;
    if (list && m.file.size() > 0) {
        rooms::Choice c;
        if (m.file.Get(static_cast<std::size_t>(m.sel), c)) {
            for (const std::string& line : Wrap(c.name.empty() ? c.host : c.name, kBottomW - 24, 3)) {
                Text(scr, 12, y, kText, line);
                y += kLineH;
            }
            y += 6;
            Text(scr, 12, y, kDim, "Address");
            Text(scr, 90, y, kText, Fit(c.host + ":" + std::to_string(c.port), kBottomW - 102));
            y += kLineH + 2;
            Text(scr, 12, y, kDim, "Password");
            Text(scr, 90, y, kText, c.password.empty() ? "none (asked if needed)" : "kept");
            y += kLineH + 2;
            const rooms::Seen seen = RowSeen(m, static_cast<std::size_t>(m.sel), c);
            Text(scr, 12, y, kDim, "Ping");
            if (seen.ping >= 0) {
                Text(scr, 90, y, PingColor(static_cast<u32>(seen.ping)), std::to_string(seen.ping) + " ms");
                Text(scr, 170, y, kDim, "Players");
                Text(scr, 240, y, kText, PlayersText(seen));
            } else {
                Text(scr, 90, y, seen.ping == -1 ? kOff : kDim, seen.ping == -1 ? "offline" : "not measured");
            }
            y += kLineH + 8;
            // Where the ping comes from, and how to have a fresh one.
            const std::string age = Age(m.file.measured_at());
            const std::string info = age.empty() ? "No ping yet: open Public lobbies in the 3DS Link app to measure the rooms."
                                                 : "Ping and players measured by the 3DS Link app " + age +
                                                       ". For fresh ones open Public lobbies in the app.";
            for (const std::string& line : Wrap(info, kBottomW - 24, 4)) {
                Text(scr, 12, y, kDim, line);
                y += kLineH;
            }
        }
    } else if (!m.active) {
        Text(scr, 12, y, kDim, "The plugin stayed out of this game:");
        Text(scr, 12, y + kLineH, kDim, "the real local wireless works.");
    } else if (m.state == RoomClient::State::Joined) {
        for (const std::string& line : Wrap(m.info.name, kBottomW - 24, 2)) {
            Text(scr, 12, y, kText, line);
            y += kLineH;
        }
        y += 4;
        std::string players = std::to_string(m.info.members.size());
        if (m.info.member_slots > 0) {
            players += " / " + std::to_string(m.info.member_slots);
        }
        Text(scr, 12, y, kDim, "Players");
        Text(scr, 80, y, kText, players);
        Text(scr, 170, y, kDim, "Ping");
        Text(scr, 210, y, PingColor(m.rtt_ms), m.rtt_ms > 0 ? std::to_string(m.rtt_ms) + " ms" : "-");
        y += kLineH + 4;
        Rect(scr, 8, y, kBottomW - 16, 1, kLine);
        y += 5;
        int shown = 0;
        for (const room::MemberInfo& member : m.info.members) {
            if (y > 196) {
                Text(scr, 12, y, kDim, "+" + std::to_string(static_cast<int>(m.info.members.size()) - shown) + " more");
                break;
            }
            const std::string nick = Fit(member.nickname, 110);
            Text(scr, 12, y, kText, nick);
            Text(scr, 130, y, member.game_name.empty() ? kDim : kText,
                 Fit(member.game_name.empty() ? "(not playing)" : member.game_name, kBottomW - 142));
            y += kLineH;
            ++shown;
        }
    } else {
        Text(scr, 12, y, kDim, "Not in a room.");
        y += kLineH + 4;
        for (const std::string& line : Wrap(m.status, kBottomW - 24, 3)) {
            Text(scr, 12, y, kText, line);
            y += kLineH;
        }
    }
    // The keys.
    Rect(scr, 0, 214, kBottomW, 26, kPanel);
    int x = 8;
    x = KeyHint(scr, x, 218, "A", m.view == View::Main ? "Choose" : m.view == View::Choose ? "Open" : "Join");
    x = KeyHint(scr, x, 218, "B", m.view == View::Main ? "Close" : "Back");
    if (m.view == View::Public) {
        KeyHint(scr, x, 218, "L/R", "Sort");
    }
}

// The list keeps the cursor on the screen.
void Scroll(Menu& m) {
    const int visible = 5; // (the rows of a list on the top screen: DrawTop)
    if (m.sel < m.first) {
        m.first = m.sel;
    } else if (m.sel >= m.first + visible) {
        m.first = m.sel - visible + 1;
    }
}

#ifdef AZL_MENU_TOUR
// A test build only (make EXTRA_CFLAGS=-DAZL_MENU_TOUR): 15 s into the game the menu opens by itself, takes the keys of kTour instead of
// the pad's and writes both screens to sdmc:/azl-shots/plugin-NAME-top.bmp and -bottom.bmp where a step says so; then it closes.
struct TourStep {
    int rounds; // rounds of the menu before the step
    u32 key;
    const char* shot;
};
const TourStep kTour[] = {
    {45, 0, "01-main"},
    {10, Key::A, nullptr},        {30, 0, "02-choose"},
    {10, Key::A, nullptr},        {30, 0, "03-favourites"},
    {10, Key::DPadDown, nullptr}, {30, 0, "04-favourite-cursor"},
    {10, Key::B, nullptr},        {10, Key::DPadDown, nullptr}, {10, Key::A, nullptr}, {30, 0, "05-public"},
    {10, Key::DPadDown, nullptr}, {10, Key::DPadDown, nullptr}, {30, 0, "06-public-cursor"},
    {10, Key::L, nullptr},        {30, 0, "06b-public-by-name"},
    {10, Key::B, nullptr},        {10, Key::DPadUp, nullptr},   {10, Key::A, nullptr}, {10, Key::DPadDown, nullptr},
    {10, Key::A, nullptr},        {240, 0, "07-other-room"},
    {10, Key::DPadDown, nullptr}, {30, 0, "08-leave-row"},
    {10, Key::DPadDown, nullptr}, {10, Key::DPadDown, nullptr}, {30, 0, "09-chat-position"},
};
std::size_t g_tour_step = 0;
int g_tour_rounds = 0;
const char* g_tour_shot = nullptr;
bool g_tour_done = false;

u32 TourKeys() {
    if (g_tour_step >= sizeof(kTour) / sizeof(kTour[0])) {
        g_tour_done = true;
        return Key::B; // (back to the main view, and closed)
    }
    const TourStep& s = kTour[g_tour_step];
    if (++g_tour_rounds < s.rounds) {
        return 0;
    }
    g_tour_rounds = 0;
    ++g_tour_step;
    g_tour_shot = s.shot;
    return s.key;
}

void TourShot() {
    if (!g_tour_shot) {
        return;
    }
    TourShotScreens(g_tour_shot); // (Sources/game_menu_tour.cpp)
    g_tour_shot = nullptr;
}
#endif

// The keys of this round: the ones just pressed, and Up / Down again while held (after 400 ms, every 90 ms).
u32 Keys(Menu& m) {
#ifdef AZL_MENU_TOUR
    return TourKeys();
#endif
    Controller::Update();
    u32 down = Controller::GetKeysPressed();
    const u32 held = Controller::GetKeysDown();
    const u64 now = osGetTime();
    const u32 nav = held & (Key::DPadUp | Key::DPadDown);
    if (down & (Key::DPadUp | Key::DPadDown)) {
        m.repeat_key = down & (Key::DPadUp | Key::DPadDown);
        m.repeat_ms = now + 400;
    } else if (nav && nav == m.repeat_key && now >= m.repeat_ms) {
        down |= nav;
        m.repeat_ms = now + 90;
    } else if (!nav) {
        m.repeat_key = 0;
    }
    return down;
}

void Run(bool active, const GameMenuHooks& hooks) {
    Log("menu: opened");
    Process::Pause();
    Menu m;
    m.active = active;
    Refresh(m, true);
    m.refusal_asked = m.state == RoomClient::State::Idle && m.error == RoomClient::Error::WrongPassword; // (a refusal from before the menu opened: Join tries again, and asks)
    for (;;) {
        if (m.osk) {
            FeedKeyboard(m, hooks);
        } else {
            const u32 down = Keys(m);
            if (!Handle(m, down, hooks)) {
                break;
            }
        }
        if (active) {
            hooks.tick();
        }
        Refresh(m, false);
        const bool refused = m.state == RoomClient::State::Idle && m.error == RoomClient::Error::WrongPassword;
        if (active && refused && !m.refusal_asked && !m.osk) {
            AskPassword(m);
        } else if (!refused) {
            m.refusal_asked = false;
        }
        Scroll(m);
        DrawTop(m, OSD::GetTopScreen());
        if (m.osk) {
            DrawKeyboard(m, OSD::GetBottomScreen());
        } else {
            DrawBottom(m, OSD::GetBottomScreen());
        }
#ifdef AZL_MENU_TOUR
        TourShot();
#endif
        OSD::SwapBuffers();
        svcSleepThread(16 * 1000 * 1000LL);
    }
    m.file.Clear();
    delete m.osk; // (the loop ends only with the keyboard closed: a guard)
    m.osk = nullptr;
    // (B is let go before the game runs again: a B still held would reach the game)
    for (int i = 0; i < 60; ++i) {
        Controller::Update();
        if (!(Controller::GetKeysDown() & Key::B)) {
            break;
        }
        svcSleepThread(16 * 1000 * 1000LL);
    }
    // (A HOME pressed while the menu was open is the game's as soon as it runs: it is never taken away from it. An SDK title's thread of
    // the notifications panics on an empty one - the crash of Super Street Fighter IV 3D on two consoles, 2026-11-04.)
    Process::Play();
    Log("menu: closed");
}

} // namespace

bool PollGameMenu(u32 chord, bool active, const GameMenuHooks& hooks, bool may_open) {
#ifdef AZL_MENU_TOUR
    static int frames = 0;
    if (!g_tour_done && ++frames == 900) {
        Run(active, hooks);
        return true;
    }
    return false;
#endif
    // The chord as a whole goes from "not all held" to "all held": the state of the pad is read here (an Update more in a frame changes
    // nothing of what is held, whoever read it last), so the opening does not depend on when the framework reads it.
    static bool was_held = true; // (a chord held while the game starts does not open the menu)
    Controller::Update();
    const bool held = (Controller::GetKeysDown() & chord) == chord;
    const bool opens = held && !was_held;
    was_held = held;
    // (the input mirror's page back before the pause, on this thread: a way back of the keep-alive amid the pause's start would read it wrong)
    if (opens && may_open && MirrorBeforePause()) {
        Run(active, hooks);
        was_held = true; // (the chord still held after B does not open it again)
        return true;
    }
    return false;
}

} // namespace ctr
} // namespace azl
