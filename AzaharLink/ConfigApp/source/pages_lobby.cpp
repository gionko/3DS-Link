// Page: Public rooms. The room list of an Azahar lobby server (the web API address that Azahar's own lobby browser uses): the app asks it for
// GET /lobby, shows the rooms, and A on one tests it and makes it the room 3DS Link joins (the same flow as a room added by hand, so the saved
// list in servers.txt gets it too; a full list gives up the room used the longest ago). The plugin is untouched: it still reads host, port and
// password from AzaharLink.ini, so nothing of this costs the game any memory.
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

#include "lobby_fetch.hpp"
#include "lobby_json.hpp"
#include "paths.hpp"
#include "pages.hpp"

using namespace ui;

namespace {

constexpr const char* kDefaultLobbyUrl = "http://88.198.47.46:5000";
constexpr u64 kTimeoutMs = 15000;

std::string LobbyFile() {
    return g.paths.datadir + "/lobby.txt";
}

std::string LoadLobbyUrl() {
    std::string url;
    if (FILE* f = std::fopen(LobbyFile().c_str(), "rb")) {
        char buf[200] = {0};
        const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        url = TrimSpaces(std::string(buf, n));
    }
    return url.empty() ? std::string(kDefaultLobbyUrl) : url;
}

bool SaveLobbyUrl(const std::string& url) {
    FILE* f = std::fopen(LobbyFile().c_str(), "wb");
    if (!f) {
        return false;
    }
    const bool ok = std::fwrite(url.data(), 1, url.size(), f) == url.size();
    return std::fclose(f) == 0 && ok;
}

// The list for the plugin's menu in the game (public_rooms.txt next to AzaharLink.ini: the plugin's folder is fixed, the app's may not be).
// A failure is silent: the page works without it.
void SavePublicRooms(const std::vector<lobby::Room>& rooms) {
    const std::string text = lobby::PublicRoomsText(rooms);
    const std::string dir = setup::kPluginDataDir;
    mkdir(dir.c_str(), 0777);
    if (FILE* f = std::fopen((dir + "/public_rooms.txt").c_str(), "wb")) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
}

bool HasNoCase(const std::string& text, const char* word) {
    const std::size_t n = std::strlen(word);
    if (n == 0 || text.size() < n) {
        return false;
    }
    for (std::size_t i = 0; i + n <= text.size(); ++i) {
        std::size_t k = 0;
        while (k < n && std::tolower(static_cast<unsigned char>(text[i + k])) == word[k]) {
            ++k;
        }
        if (k == n) {
            return true;
        }
    }
    return false;
}

bool LooksLikeMonsterHunter(const lobby::Room& r) {
    for (const std::string* s : {&r.name, &r.preferred_game, &r.description}) {
        if (HasNoCase(*s, "monster") || HasNoCase(*s, "mh4") || HasNoCase(*s, "mhx") || HasNoCase(*s, "mhgen") || HasNoCase(*s, "mh3")) {
            return true;
        }
    }
    return false;
}

// The name a saved room can carry: up to 12 of A-Z a-z 0-9 space . _ - (what the list allows), from the lobby's long title.
std::string ShortName(const std::string& title) {
    std::string out;
    bool space = true; // (no leading space, no double space)
    for (char c : title) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (ok) {
            out.push_back(c);
            space = false;
        } else if (c == ' ' && !space) {
            out.push_back(' ');
            space = true;
        }
        if (out.size() >= setup::kMaxServerNameChars) {
            break;
        }
    }
    out = TrimSpaces(out);
    return setup::ValidServerName(out) ? out : std::string();
}

class LobbyPage : public ListPage {
public:
    LobbyPage() : url_(LoadLobbyUrl()) {
        Refresh();
    }
    ~LobbyPage() override {
        fetch_.Abandon();
    }

protected:
    const char* Title() const override {
        return "Public rooms";
    }
    const char* BarTitle() const override {
        return "Public rooms";
    }
    std::string BarRight() const override {
        if (state_ == State::Loading) {
            return "Loading...";
        }
        if (state_ == State::Failed) {
            return "Not loaded";
        }
        return Fmt("%u room%s", static_cast<unsigned>(shown_), shown_ == 1 ? "" : "s");
    }
    const char* ActionLabel() const override {
        const Row* row = Selected();
        return row && row->id == kUrl ? "Edit" : "Use";
    }
    bool StartButton() const override {
        return false;
    }
    int ExtraKeys(FooterKey* out) const override {
        out[0] = FooterKey{"X", kFilterNames[filter_], KEY_X};
        out[1] = FooterKey{"Y", "Refresh", KEY_Y};
        return 2;
    }
    void OnExtra(int index, const Row*) override {
        if (index == 0) {
            filter_ = (filter_ + 1) % 3;
            list_.sel = 1;
            list_.follow = true;
        } else {
            Refresh();
        }
    }

    void Update(const Input& in) override {
        Poll();
        ListPage::Update(in);
    }

    void BuildRows(std::vector<Row>& rows) override {
        rows.clear();
        Row url;
        url.id = kUrl;
        url.kind = Kind::Value;
        url.label = "Lobby";
        url.value = gfx::Clean(url_);
        url.note_title = "Lobby address";
        url.note = "The web API address of a lobby server (Azahar's \"web API URL\"). The app asks it for /lobby. A: change it. Y: load the list again.";
        rows.push_back(url);

        setup::Server active;
        active.host = g.cfg.host;
        active.port = g.cfg.port;
        shown_ = 0;

        if (state_ == State::Loading) {
            Row r;
            r.id = kInfo;
            r.kind = Kind::Info;
            r.label = "Loading the room list";
            r.busy = true;
            r.gap = 6.0f;
            rows.push_back(r);
            return;
        }
        if (state_ == State::Failed) {
            Row r;
            r.id = kInfo;
            r.kind = Kind::Info;
            r.label = "Could not load the list";
            r.label_color = col::kOff;
            r.gap = 6.0f;
            r.note_title = "What happened";
            r.note = error_ + "\nY: try again. A on the first row: another address.";
            rows.push_back(r);
            return;
        }
        for (std::size_t i = 0; i < rooms_.size(); ++i) {
            const lobby::Room& room = rooms_[i];
            if (!Passes(room)) {
                continue;
            }
            Row row;
            row.id = kRoomBase + static_cast<int>(i);
            row.kind = Kind::Action;
            row.label = gfx::Clean(TrimSpaces(room.name));
            row.value = room.max_players ? Fmt("%u/%u", room.players, room.max_players) : Fmt("%u", room.players);
            if (room.has_password) {
                row.chip = "pw";
                row.chip_color = col::kWarn;
            } else if (room.net_version != azl::room::kNetworkVersion) {
                row.chip = Fmt("v%u", room.net_version);
                row.chip_color = col::kWarn;
            }
            setup::Server s;
            s.host = room.address;
            s.port = static_cast<u16>(room.port);
            row.mark = s.Same(active) ? Mark::Dot : Mark::Slot;
            if (shown_ == 0) {
                row.gap = 6.0f;
            }
            row.note_title = row.label;
            std::string who;
            for (std::size_t k = 0; k < room.nicknames.size() && k < 4; ++k) {
                who += (k ? ", " : "") + room.nicknames[k];
            }
            row.note = "Game: " + gfx::Clean(room.preferred_game.empty() ? "no preference" : room.preferred_game) + "\n" +
                       Fmt("Address: %s:%u\n", gfx::Clean(room.address).c_str(), room.port) +
                       (who.empty() ? std::string("Nobody in it now.\n") : "Players: " + gfx::Clean(who) + "\n") +
                       (room.has_password ? "Password protected: A asks for it.\n" : "") +
                       (room.net_version != azl::room::kNetworkVersion ? Fmt("Protocol v%u (this app speaks v%u).\n", room.net_version,
                                                                              static_cast<unsigned>(azl::room::kNetworkVersion))
                                                                        : std::string());
            rows.push_back(row);
            ++shown_;
        }
        if (shown_ == 0) {
            Row r;
            r.id = kInfo;
            r.kind = Kind::Info;
            r.label = rooms_.empty() ? "The lobby has no rooms now" : "No room matches this filter (X)";
            r.gap = 6.0f;
            rows.push_back(r);
        }
    }

    void Activate(const Row& row, int step) override {
        if (step != 0) {
            return;
        }
        if (row.id == kUrl) {
            ChangeUrl();
        } else if (row.id >= kRoomBase) {
            Choose(static_cast<std::size_t>(row.id - kRoomBase));
        }
    }

private:
    enum Id { kUrl = 1, kInfo = 2, kRoomBase = 100 };
    enum class State { Loading, Ready, Failed };
    static constexpr const char* kFilterNames[3] = {"All", "Monster Hunter", "Open"};

    bool Passes(const lobby::Room& r) const {
        switch (filter_) {
        case 1:
            return LooksLikeMonsterHunter(r);
        case 2:
            return !r.has_password && (r.max_players == 0 || r.players < r.max_players);
        default:
            return true;
        }
    }

    void Refresh() {
        state_ = State::Loading;
        rooms_.clear();
        error_.clear();
        fetch_.Start(url_);
        list_.sel = 0;
    }

    void Poll() {
        if (state_ != State::Loading) {
            return;
        }
        const lobby::Fetch::Phase phase = fetch_.phase();
        if (phase == lobby::Fetch::Phase::Done) {
            std::string why;
            if (lobby::Parse(fetch_.body(), rooms_, &why)) {
                // the rooms with people first (a stable sort keeps the server's order inside each group)
                std::stable_sort(rooms_.begin(), rooms_.end(), [](const lobby::Room& a, const lobby::Room& b) { return a.players > b.players; });
                state_ = State::Ready;
                SavePublicRooms(rooms_);
            } else {
                error_ = why;
                state_ = State::Failed;
            }
        } else if (phase == lobby::Fetch::Phase::Failed) {
            error_ = fetch_.error();
            state_ = State::Failed;
        } else if (fetch_.elapsed_ms() > kTimeoutMs) {
            fetch_.Abandon();
            error_ = "No answer in 15 seconds. Is WiFi on, and is the address right?";
            state_ = State::Failed;
        }
    }

    void ChangeUrl() {
        std::string input = url_;
        if (g.EditText("Lobby address, like http://88.198.47.46:5000", input, 120, false)) {
            input = TrimSpaces(input);
            if (input.empty()) {
                return;
            }
            url_ = input;
            if (!SaveLobbyUrl(url_)) {
                g.ShowToast("Could not save the address.", col::kWarn, 4000);
            }
            Refresh();
        }
    }

    // A on a room: its password when it has one, a question when its protocol is another, then the same test as a room added by hand.
    void Choose(std::size_t index) {
        if (index >= rooms_.size()) {
            return;
        }
        const lobby::Room& room = rooms_[index];
        setup::Server server;
        if (setup::ParseServer(room.address + ":" + std::to_string(room.port), server) != setup::ServerParse::Ok) {
            g.ShowToast("This room has an address the app cannot use.", col::kOff, 5000);
            return;
        }
        server.name = ShortName(room.name);
        if (room.has_password) {
            std::string pass;
            if (!g.EditText("Password of the room", pass, 64, true)) {
                return;
            }
            server.password = pass;
        }
        if (room.net_version != azl::room::kNetworkVersion) {
            PushPage(pages::NewConfirmPage("Another protocol",
                                           Fmt("This room speaks protocol v%u and 3DS Link v%u. It probably will not work. Test it anyway?", room.net_version,
                                               static_cast<unsigned>(azl::room::kNetworkVersion)),
                                           "Yes, test it", "No", [this, server](bool yes) {
                                               if (yes) {
                                                   Test(server);
                                               }
                                           }));
            return;
        }
        Test(server);
    }

    void Test(const setup::Server& server) {
        PushPage(pages::NewProbePage(server, [this, server](pages::ProbeOutcome outcome) {
            if (outcome == pages::ProbeOutcome::Reached) {
                Select(server);
            } else if (outcome == pages::ProbeOutcome::Failed) {
                PushPage(pages::NewConfirmPage("Connection test failed", "Could not reach the room. Use it as the active room anyway?",
                                               "Yes, use it", "No", [this, server](bool yes) {
                                                   if (yes) {
                                                       Select(server);
                                                   }
                                               }));
            } else {
                g.ShowToast("Test cancelled.", col::kText, 3000);
            }
        }));
    }

    // The ini keeps the room 3DS Link joins (with its password); servers.txt only remembers it (a full list drops its oldest).
    void Select(const setup::Server server) {
        g.cfg.host = server.host;
        g.cfg.port = server.port;
        g.cfg.password = server.password;
        g.Save();
        if (g.save_ok) {
            g.ShowToast("Room: " + gfx::Clean(server.Label()), col::kOn);
        }
        if (g.servers.Use(server)) {
            g.SaveServers();
        }
    }

    lobby::Fetch fetch_;
    std::string url_;
    State state_ = State::Loading;
    std::string error_;
    std::vector<lobby::Room> rooms_;
    int filter_ = 0;
    std::size_t shown_ = 0;
};

} // namespace

namespace pages {

ui::Page* NewLobbyPage() {
    return new LobbyPage;
}

} // namespace pages
