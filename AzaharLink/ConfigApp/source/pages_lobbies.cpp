// Page: Public lobbies (from Azahar Emulator Room). The public Azahar rooms of the list on GitHub, each asked without joining it (lobby_scan.cpp):
// its ping, who is in it and what they play, a password, a full room. A uses a room (after the test of a room, with its password: asked when it
// has one we do not know, and kept), X adds it to the favourites (the Server list, where only its name can change), Y searches, SELECT asks the
// rooms again, R and L filter (the region, the kind), START is Save & exit as on every page. The top screen is the room under the cursor (who
// is in it, whether the user has the game), the keys as buttons and how many rooms answered; the warning about public rooms is on the row of
// the Server list that opens this page. What is decided is in public_lobbies.hpp (tested on a PC).
#include <algorithm>
#include <cctype>
#include <ctime>

#include "copyworker.hpp"
#include "lobby_scan.hpp"
#include "pages.hpp"
#include "plugin_install.hpp"
#include "system.hpp"
#include "public_lobbies.hpp"
#include "titles.hpp"
#include "tour.hpp"
#include "update_worker.hpp"

using namespace ui;

namespace pages {

namespace {

using lobbies::Entry;
using lobbies::Probe;

constexpr u64 kRebuildMs = 500;   // while the answers come, the rows are made again at most this often (the order moves with them)
constexpr u64 kRetryMs = 1000;    // a job that could not start yet (the update's thread was busy) is tried again
constexpr u64 kRefreshPauseMs = 10000; // SELECT asks every room again at most this often (every ask reaches the games being played in it)

// What the last passes of this run said, by room: a page opened again shows it at once (faint) until the new answers come.
std::vector<std::pair<std::string, Probe>>& LastSeen() {
    static std::vector<std::pair<std::string, Probe>> last;
    return last;
}

const Probe* FindLastSeen(const std::string& key) {
    for (const auto& kv : LastSeen()) {
        if (kv.first == key) {
            return &kv.second;
        }
    }
    return nullptr;
}

void RememberSeen(const std::string& key, const Probe& p) {
    for (auto& kv : LastSeen()) {
        if (kv.first == key) {
            kv.second = p;
            return;
        }
    }
    LastSeen().emplace_back(key, p);
}

// titles.txt, read once a run (Load) or when a new one came (ApplyWaiting).
struct TitlesStore {
    lobbies::Titles titles;
    bool loaded = false;
};
TitlesStore& TitlesCache() {
    static TitlesStore store;
    return store;
}

std::string ActiveKey() {
    lobbies::Room r;
    r.host = g.cfg.host;
    r.port = g.cfg.port;
    return g.cfg.host.empty() ? std::string() : lobbies::RoomKey(r);
}

bool ActiveIsSaved() {
    setup::Server active;
    active.host = g.cfg.host;
    active.port = g.cfg.port;
    return !g.cfg.host.empty() && g.servers.Find(active) >= 0;
}

lobbies::List ReadList() {
    std::string text;
    lobbies::List list;
    if (setup::ReadTextFile(g.paths.lobbies_file, lobbies::kMaxListBytes + 1, text) == setup::LoadResult::Loaded) {
        lobbies::ParseList(text, list);
    }
    return list;
}

// `ok` false: the file is there but could not be read. It is then never written over (the user's passwords are in it).
lobbies::Local ReadLocal(bool* ok = nullptr) {
    std::string text;
    const setup::LoadResult r = setup::ReadTextFile(g.paths.lobbies_local, lobbies::kMaxLocalBytes + 1, text);
    if (ok) {
        *ok = r != setup::LoadResult::Unreadable;
    }
    return lobbies::ParseLocal(text);
}

// What waits in <file>.new (update_worker.cpp, the job of the lists): "" when nothing waits.
std::string Waiting(const std::string& path, std::size_t max_bytes) {
    std::string text;
    if (setup::ReadTextFile(path + ".new", max_bytes + 1, text) != setup::LoadResult::Loaded) {
        return std::string();
    }
    return text;
}

u32 PingColor(int ping_ms) {
    switch (lobbies::TierOf(ping_ms)) {
    case lobbies::PingTier::Blue:
        return gfx::Rgba(86, 150, 255);
    case lobbies::PingTier::Green:
        return col::kOn;
    case lobbies::PingTier::Yellow:
        return col::kWarn;
    default:
        return col::kOff;
    }
}

// "Yes, enable it": a popup that waits for the copy of the plugin into the title's folder and takes no key meanwhile (B, START, the touch
// screen: leaving the app in the middle of the copy is what it is there against; HOME is off during every copy anyway, main.cpp). Then a
// message says how it went. It stays up a moment at least, so that it is seen.
class EnablingPage : public Page {
public:
    EnablingPage(u64 tid, const std::string& game_name) : tid_(tid), name_(game_name), since_(NowMs()) {}

    void Update(const Input&) override {
        setup::CopyWorker& worker = setup::CopyWorker::Get();
        setup::CopyDone done;
        while (worker.TakeDone(done)) {
            // (the other results of the worker are no news here: Enable Titles reads the switches from the card)
            if (done.tid == tid_ && done.kind == setup::CopyKind::Enable) {
                result_ = done.enable_result;
                got_ = true;
            }
        }
        if (!got_ && !worker.Pending(tid_, nullptr)) {
            // (over, and its result taken by somebody else: what the card says)
            result_ = setup::ReadSlot(tid_).slot == setup::Slot::On ? setup::EnableResult::Ok : setup::EnableResult::CopyFailed;
            got_ = true;
        }
        if (!got_ || NowMs() - since_ < kMinShownMs) {
            return;
        }
        PopPage();
        switch (result_) {
        case setup::EnableResult::Ok:
            PushPage(pages::NewMessagePage("Game enabled", name_ + " is enabled for 3DS Link: start it and play in this room."));
            break;
        case setup::EnableResult::OtherPlugin:
            PushPage(pages::NewMessagePage("Not enabled", "Another plugin is in the folder of " + name_ + ": remove it first."));
            break;
        case setup::EnableResult::SourceMissing:
            PushPage(pages::NewMessagePage("Not enabled", "AzaharLink.3gx is gone from " + g.paths.appdir + ": open Enable Titles, it puts it back."));
            break;
        case setup::EnableResult::CopyFailed:
            PushPage(pages::NewMessagePage("Not enabled", "Could not copy the plugin for " + name_ + " (is the card full or locked?)."));
            break;
        }
    }

    void DrawTop() override {
        if (Page* below = PageBelow()) {
            below->DrawTop();
        }
        DrawDim(gfx::kTopW);
    }

    void DrawBottom() override {
        if (Page* below = PageBelow()) {
            below->DrawBottom();
        }
        DrawDim(gfx::kBottomW);
        DrawPopupCard(60, 120, "Enabling the game", "Copying the plugin for " + name_ + ". Please wait: do not leave the app now.", 3);
        DrawSpinner(158);
        DrawFrameBorder(gfx::kBottomW);
    }

private:
    static constexpr u64 kMinShownMs = 800;
    u64 tid_;
    std::string name_;
    u64 since_;
    bool got_ = false;
    setup::EnableResult result_ = setup::EnableResult::CopyFailed;
};

class LobbiesPage : public ListPage {
public:
    LobbiesPage() {
        Load();
        StartScan();
    }
    ~LobbiesPage() override {
        lobbies::Scanner::Get().Stop(); // (it ends by itself in a moment; the app waits for it only at its end)
        gfx::ReleaseIcons();             // (the icon of the user's game on the top screen)
    }

    void Update(const Input& in) override {
        const u64 now = NowMs();
        PollLists(now);
        PollScan(now);
        PollTitles();
        if (!gone_.empty()) {
            // (a page pushed from the constructor would be replaced by this one: the message comes at the first frame)
            PushPage(pages::NewMessagePage("The public room is gone",
                                           "The room you were using, " + gfx::Clean(gone_) + ", is no longer among the public rooms. Choose another room."));
            gone_.clear();
        }
        if (titles::ScanGeneration() != scan_generation_) {
            // (a new scan of the titles: the icons and the titles of the old one are gone)
            scan_generation_ = titles::ScanGeneration();
            owned_.clear();
            owned_icons_.clear();
            owned_names_.clear();
            owned_ready_ = false;
            dirty_ = rows_dirty_ = true;
        }
        // Not while the list has a touch open, the frame it ends in included: a row that moved under the stylus would be the one chosen. A new
        // list cannot wait (the rows are other rooms now): the touch is dropped.
        if (dirty_ && force_ && list_.touching) {
            list_.touching = false;
            list_.dragging = false;
            list_.press_row = -1;
        }
        if (dirty_ && (force_ || (!list_.touching && now - built_ms_ >= kRebuildMs))) {
            Arrange();
            built_ms_ = now;
            force_ = false;
            dirty_ = false;
            rows_dirty_ = true;
        }
        ListPage::Update(in);
    }

    void DrawTop() override;
    void DrawBottom() override {
        if (const Row* row = Selected()) {
            if (row->id >= kRoomBase && static_cast<std::size_t>(row->id - kRoomBase) < entries_.size()) {
                cursor_key_ = lobbies::RoomKey(entries_[static_cast<std::size_t>(row->id - kRoomBase)].room);
            }
        }
        ListPage::DrawBottom();
        // the labels of the columns, on the bar
        const u32 dim = gfx::Mix(col::kBarText, col::kBar, 0.35f);
        gfx::DrawText(kLobbyPingRight, kBarH * 0.5f, 0.42f, dim, "Ping", gfx::Align::Right);
        gfx::DrawText(kLobbyPlayersRight, kBarH * 0.5f, 0.42f, dim, "Players", gfx::Align::Right);
    }

protected:
    const char* Title() const override {
        return "Public lobbies";
    }
    const char* BarTitle() const override {
        return "Name";
    }
    // (the "HOME off" badge, during the pass of the plugin over the games, goes before the labels of the columns, not over them)
    float BarBadgeRight() const override {
        return kLobbyPingRight - gfx::TextWidth("Ping", 0.42f) - 8.0f;
    }
    const char* ActionLabel() const override {
        const Row* row = Selected();
        if (!row || row->id == kInfo) {
            return "";
        }
        return row->id == kQuick ? "Join" : "Use";
    }
    // (no buttons after A in the footer: START stays Save & exit, as on every page; the other keys are buttons on the top screen)
    void OnKeys(const Input& in) override {
        if (in.down & KEY_R) {
            region_ = (region_ + 1) % lobbies::kRegionFilterCount;
            Refiltered();
        } else if (in.down & KEY_L) {
            kind_ = static_cast<lobbies::Kind>((static_cast<int>(kind_) + 1) % lobbies::kKinds);
            Refiltered();
        } else if (in.down & KEY_SELECT) {
            Refresh();
        } else if (in.down & KEY_Y) {
            std::string query = search_;
            if (g.EditText("Search the rooms: a name or a game (a letter wrong is fine)", query, 40, true)) {
                search_ = TrimSpaces(query);
                Refiltered();
            }
        } else if (in.down & KEY_X) {
            const Row* row = Selected();
            if (row && row->id >= kRoomBase) {
                Keep(static_cast<std::size_t>(row->id - kRoomBase));
            }
        }
    }
    void OnBack() override {
        if (!search_.empty()) {
            search_.clear(); // (B takes the search away first)
            Refiltered();
            return;
        }
        PopPage();
    }

    void BuildRows(std::vector<Row>& rows) override {
        if (!rows_dirty_) {
            return; // (the rows are made in Arrange: a list of a few hundred rooms is not made again every frame)
        }
        rows_dirty_ = false;
        rows.clear();
        Row quick;
        quick.id = kQuick;
        quick.kind = Kind::Action;
        quick.label = "Quick join";
        quick.label_color = col::kAccent;
        quick.mark = Mark::Slot;
        quick.note_title = "Quick join";
        quick.note = "Takes the room with a free slot where somebody plays one of your games (your edition first), with the lowest ping, and joins "
                     "it the way A does.";
        rows.push_back(quick);
        if (entries_.empty()) {
            Row info;
            info.id = kInfo;
            info.kind = Kind::Info;
            info.gap = 6.0f;
            info.label = lists_failed_ ? "No list of public rooms yet" : "Getting the list of public rooms";
            info.busy = !lists_failed_;
            info.note_title = "The list of public rooms";
            info.note = "It comes from GitHub (gionko/3DS-Link-Public-Lobbies) when this page opens, and is kept on the card. This console needs "
                        "the WiFi, internet and the right date for it.";
            rows.push_back(info);
            return;
        }
        if (order_.empty()) {
            Row info;
            info.id = kInfo;
            info.kind = Kind::Info;
            info.gap = 6.0f;
            info.label = "No room matches";
            info.note_title = "No room matches";
            info.note = "Change the filters (R: the region, L: all, your games, free slots) or the search (Y; B takes it away).";
            rows.push_back(info);
            return;
        }
        const std::string active = ActiveKey();
        bool first = true;
        for (std::size_t idx : order_) {
            const Entry& e = entries_[idx];
            const Probe& p = probes_[idx];
            Row row;
            row.id = kRoomBase + static_cast<int>(idx);
            row.kind = Kind::Action;
            row.lobby = true;
            row.gap = first ? 6.0f : 0.0f;
            first = false;
            row.label = gfx::Clean(e.room.name.empty() ? e.room.host : e.room.name);
            row.mark = lobbies::RoomKey(e.room) == active ? Mark::Dot : Mark::Slot;
            // (no icon on the row: the name needs the room; the user's game is said on the top screen, with its icon)
            lobbies::Match match = lobbies::Match::None;
            RoomOwned(p, &match);
            const bool other_region = match == lobbies::Match::OtherRegion;
            const bool stale = !fresh_[idx];
            if (match != lobbies::Match::None) {
                row.label_color = col::kOn; // (one of the user's games is played in it, of their region or of another: the square says which)
            }
            if (p.state == lobbies::State::Offline) {
                row.label_color = col::kOff;
                row.ping = "offline";
                row.ping_color = col::kOff;
            } else if (p.state == lobbies::State::Online) {
                row.ping = Fmt("%d", p.ping_ms);
                row.ping_color = stale ? col::kDim : PingColor(p.ping_ms);
                if (p.has_info && p.slots > 0) {
                    row.players = Fmt("%u/%u", static_cast<unsigned>(p.members.size()), static_cast<unsigned>(p.slots));
                    row.fill = static_cast<float>(p.members.size()) / static_cast<float>(p.slots);
                } else if (p.has_info) {
                    row.players = Fmt("%u", static_cast<unsigned>(p.members.size()));
                } else {
                    row.players = "-";
                }
            } else {
                row.ping = "...";
                row.ping_color = col::kDim;
            }
            row.dim = stale && p.state != lobbies::State::Offline;
            // (small squares, their legend on the top screen: every one that is true, not only the first)
            row.lobby_marks = (lobbies::IsFull(p) ? kLobbyFull : 0u) | (other_region ? kLobbyRegion : 0u) | (saved_[idx] ? kLobbyFavourite : 0u) |
                              (e.kept ? kLobbyGone : 0u) |
                              ((p.version != 0 && p.version != azl::room::kNetworkVersion) ? kLobbyVersion : 0u);
            if (p.lock == lobbies::Lock::Password) {
                row.lock = (!e.room.secret.empty() && (p.secret_ok || p.state != lobbies::State::Online)) ? 2 : 1;
            }
            rows.push_back(row);
        }
    }

    void Activate(const Row& row, int step) override {
        if (step != 0) {
            return;
        }
        if (row.id == kQuick) {
            QuickJoin();
        } else if (row.id >= kRoomBase) {
            Use(static_cast<std::size_t>(row.id - kRoomBase), false);
        }
    }

private:
    enum Id { kQuick = 1, kInfo = 2, kRoomBase = 100 };

    // The list and the user's file from the card, the titles, and the answers this run already had. A list or titles that came while no page
    // was open are applied first.
    void Load() {
        lobby_list_ = ReadList();
        local_ = ReadLocal(&local_ok_);
        if (!update::Worker::Get().ListsRunning()) {
            ApplyWaiting();
        }
        if (!TitlesCache().loaded) {
            // (titles.txt, 76 KB, is read once a run: it changes a few times a year, and a new one comes through ApplyWaiting)
            std::string titles_text;
            if (setup::ReadTextFile(g.paths.titles_file, lobbies::kMaxTitlesBytes + 1, titles_text) == setup::LoadResult::Loaded) {
                TitlesCache().titles.Parse(titles_text);
            }
            TitlesCache().loaded = true;
        }
        owned_.clear();
        owned_ready_ = false;
        scan_generation_ = titles::ScanGeneration();
        Rebuild();
    }

    bool SaveLocal() {
        if (!local_ok_) {
            g.ShowToast("Your names and passwords of the public rooms could not be read: they are not written over.", col::kOff, 5000);
            return false;
        }
        return setup::WriteTextFile(g.paths.lobbies_local, lobbies::RenderLocal(local_));
    }

    // What the job of the lists left in the .new files, put in place on this thread: a new list goes through Remember (what the user keeps
    // follows it; the room in use that left it is kept once, then it is not the room in use any more), new titles are read again. Returns
    // whether the list changed.
    bool ApplyWaiting() {
        bool list_changed = false;
        const std::string text = Waiting(g.paths.lobbies_file, lobbies::kMaxListBytes);
        lobbies::List fresh;
        if (!text.empty() && local_ok_ && lobbies::ParseList(text, fresh)) {
            // (with the user's file unreadable the list waits: what it keeps, and the room in use that left the list, are in that file)
            const lobbies::Outcome out = lobbies::Remember(local_, lobby_list_, fresh, g.cfg.host, g.cfg.port, ActiveIsSaved());
            if (setup::WriteTextFile(g.paths.lobbies_local, lobbies::RenderLocal(out.local))) {
                local_ = out.local;
                if (out.deselect) {
                    gone_ = RoomLabel();
                    g.cfg.host.clear();
                    g.cfg.password.clear();
                    g.Save();
                    g.public_room_key.clear();
                    g.public_room_name.clear();
                }
                if (setup::WriteTextFile(g.paths.lobbies_file, text)) {
                    setup::WriteTextFile(g.paths.lobbies_file + ".new", std::string());
                }
                lobby_list_ = fresh; // (what the user's file now says is about this list)
                list_changed = true;
            }
        } else if (!text.empty() && local_ok_) {
            setup::WriteTextFile(g.paths.lobbies_file + ".new", std::string()); // (not a list: dropped)
        }
        const std::string titles_text = Waiting(g.paths.titles_file, lobbies::kMaxTitlesBytes);
        lobbies::Titles titles;
        if (!titles_text.empty()) {
            if (titles.Parse(titles_text) && setup::WriteTextFile(g.paths.titles_file, titles_text)) {
                TitlesCache().titles = std::move(titles);
                TitlesCache().loaded = true;
                owned_.clear(); // (they pointed into the titles that are gone)
                owned_icons_.clear();
                owned_names_.clear();
                owned_ready_ = false;
            }
            setup::WriteTextFile(g.paths.titles_file + ".new", std::string());
        }
        return list_changed;
    }

    // The rows of the list again (a new list, a name or a password that changed): the answers stay with their rooms, the cursor with its room.
    void Rebuild() {
        std::vector<std::string> fresh_keys;
        for (std::size_t i = 0; i < entries_.size() && i < fresh_.size(); ++i) {
            if (fresh_[i]) {
                fresh_keys.push_back(lobbies::RoomKey(entries_[i].room));
            }
        }
        entries_ = lobbies::Entries(lobby_list_, local_);
        probes_.assign(entries_.size(), Probe());
        fresh_.assign(entries_.size(), false);
        saved_.assign(entries_.size(), false);
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const std::string key = lobbies::RoomKey(entries_[i].room);
            if (const Probe* seen = FindLastSeen(key)) {
                probes_[i] = *seen;
                fresh_[i] = std::find(fresh_keys.begin(), fresh_keys.end(), key) != fresh_keys.end();
            }
            setup::Server s;
            s.host = entries_[i].room.host;
            s.port = entries_[i].room.port;
            saved_[i] = g.servers.Find(s) >= 0;
        }
        dirty_ = true;
        force_ = true;
    }

    // A filter or the search changed: the cursor goes back to the first room.
    void Refiltered() {
        dirty_ = true;
        force_ = true;
        list_.sel = std::min(list_.sel, 1);
        cursor_key_.clear();
    }

    // Which rooms are shown, in which order.
    void Arrange() {
        // the room the cursor is on, by its room (the indexes of entries_ change with a new list)
        std::string cursor_key;
        if (const Row* row = Selected()) {
            if (row->id >= kRoomBase && static_cast<std::size_t>(row->id - kRoomBase) < entries_.size()) {
                cursor_key = cursor_key_;
            }
        }
        order_.clear();
        const lobbies::Region region = lobbies::kRegionFilters[region_];
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const Probe& p = probes_[i];
            if (lobbies::PassesKind(kind_, p, owned_, titles_) && lobbies::PassesRegion(region, p, titles_) &&
                lobbies::Matches(search_, entries_[i], p)) {
                order_.push_back(i);
            }
        }
        std::stable_sort(order_.begin(), order_.end(), [&](std::size_t a, std::size_t b) {
            return lobbies::Before(probes_[a], entries_[a].room.name, probes_[b], entries_[b].room.name);
        });
        // the cursor stays on the room it was on (the order moves while the answers come)
        if (!cursor_key.empty()) {
            for (std::size_t k = 0; k < order_.size(); ++k) {
                if (lobbies::RoomKey(entries_[order_[k]].room) == cursor_key) {
                    list_.sel = 1 + static_cast<int>(k);
                    break;
                }
            }
        }
    }

    void StartScan() {
        std::vector<lobbies::Target> targets;
        for (const Entry& e : entries_) {
            targets.push_back({e.room.host, e.room.port, e.room.secret});
        }
        scan_wanted_ = !lobbies::Scanner::Get().Start(targets);
        scan_gen_ = lobbies::Scanner::Get().generation();
        fresh_.assign(entries_.size(), false);
        scanning_ = !scan_wanted_;
        if (scanning_) {
            scan_started_ms_ = NowMs();
        }
    }

    void Refresh() {
        if (lobbies::Scanner::Get().running()) {
            g.ShowToast("Still asking the rooms: wait for the answers.", col::kWarn, 3000);
            return;
        }
        if (scan_started_ms_ && NowMs() - scan_started_ms_ < kRefreshPauseMs) {
            g.ShowToast("The rooms were asked a moment ago: try again in a few seconds.", col::kText, 3000);
            return;
        }
        StartScan();
        g.ShowToast("Asking the rooms again.", col::kText, 2000);
    }

    void PollScan(u64 now) {
        lobbies::Scanner& scanner = lobbies::Scanner::Get();
        if (rescan_ && !scanner.running()) {
            rescan_ = false;
            StartScan();
        }
        if (scan_wanted_ && now - scan_try_ms_ >= kRetryMs / 4) {
            scan_try_ms_ = now;
            StartScan();
        }
        if (!scanning_) {
            return;
        }
        const u32 gen = scanner.generation();
        if (gen != scan_gen_) {
            scan_gen_ = gen;
            std::vector<Probe> results;
            scanner.Read(results);
            if (results.size() == entries_.size()) {
                for (std::size_t i = 0; i < results.size(); ++i) {
                    if (results[i].state == lobbies::State::Online || results[i].state == lobbies::State::Offline) {
                        probes_[i] = results[i]; // (an answer grows: the connection first, then the room's information)
                        fresh_[i] = true;
                        RememberSeen(lobbies::RoomKey(entries_[i].room), results[i]);
                    } else if (results[i].state == lobbies::State::Asking && !FindLastSeen(lobbies::RoomKey(entries_[i].room))) {
                        probes_[i].state = lobbies::State::Asking;
                    }
                }
                dirty_ = true;
            }
        }
        if (!scanner.running()) {
            scanning_ = false;
            dirty_ = true;
            SavePings();
        }
    }

    // A pass is over: what it measured goes to public-lobbies-ping.txt, for the in-game menu (it sorts its list of public lobbies by it and
    // says how old it is; it never asks a room itself). "#measured <seconds since 1970>", then "address:port:ping:players:slots", ping -1 for
    // a room that did not answer. Only the rooms this pass asked.
    void SavePings() {
        std::string text = "# What 3DS Link measured of the public rooms, for its in-game menu (3DS Link writes this file).\n";
        text += "#measured " + std::to_string(static_cast<unsigned long long>(std::time(nullptr))) + "\n";
        for (std::size_t i = 0; i < entries_.size() && i < probes_.size() && i < fresh_.size(); ++i) {
            const Probe& p = probes_[i];
            if (!fresh_[i] || (p.state != lobbies::State::Online && p.state != lobbies::State::Offline)) {
                continue;
            }
            const bool online = p.state == lobbies::State::Online;
            text += entries_[i].room.host + ":" + std::to_string(entries_[i].room.port) + ":" + std::to_string(online ? p.ping_ms : -1) + ":" +
                    std::to_string(online ? p.members.size() : 0) + ":" + std::to_string(online ? p.slots : 0) + "\n";
        }
        setup::WriteTextFile(g.paths.lobbies_ping, text);
    }

    void PollLists(u64 now) {
        update::Worker& worker = update::Worker::Get();
        if (tour::SkipFetch()) {
            lists_started_ = lists_handled_ = true; // (a test build shows the files that were put on the card)
            return;
        }
        if (!lists_started_) {
            if (now - lists_try_ms_ < kRetryMs) {
                return;
            }
            lists_try_ms_ = now;
            lists_started_ = worker.StartLists(g.paths.lobbies_file, g.paths.titles_file);
            return;
        }
        if (lists_handled_) {
            return;
        }
        const update::ListsStatus st = worker.ReadLists();
        if (st.stage == update::ListsStatus::Stage::Running || st.stage == update::ListsStatus::Stage::Idle || worker.ListsRunning()) {
            return;
        }
        lists_handled_ = true;
        lists_failed_ = st.stage == update::ListsStatus::Stage::Failed;
        dirty_ = true;
        rows_dirty_ = true; // (the empty page says it did not come)
        if (ApplyWaiting()) {
            Rebuild();
            if (lobbies::Scanner::Get().running()) {
                rescan_ = true;       // (the pass that runs is not cut: the rooms are asked again when it ends ...)
                scanning_ = false;    // (... and its answers are not read: they are of the rooms of the old list)
            } else {
                StartScan();
            }
        }
    }

    // The user's titles, once the scan of the titles is over: what "My games", the icons and the notes compare with.
    void PollTitles() {
        if (owned_ready_ || !titles::Scan().done()) {
            return;
        }
        owned_ready_ = true;
        owned_.clear();
        owned_icons_.clear();
        owned_names_.clear();
        for (const auto& game : titles::Scan().games()) {
            lobbies::Owned o;
            o.key = lobbies::TitleKey(game->tid);
            o.title = titles_.Find(game->tid);
            owned_.push_back(o);
            owned_icons_.push_back(game->icon);
            owned_names_.push_back(game->name);
        }
        dirty_ = true;
    }

    // The user's game that the room plays (the same edition first, then the same game of the user's region, then of another region): its index
    // in owned_, or -1; `match` says which of the three.
    int RoomOwned(const Probe& p, lobbies::Match* match) const {
        int found = -1;
        int best = 99;
        *match = lobbies::Match::None;
        for (const lobbies::Member& m : p.members) {
            const lobbies::Owned* mine = nullptr;
            const lobbies::Match this_match = lobbies::MatchOf(m, owned_, titles_, &mine);
            const int rank = this_match == lobbies::Match::Same ? 0 : this_match == lobbies::Match::Family ? 1 : this_match == lobbies::Match::OtherRegion ? 2 : 99;
            if (rank < best && mine) {
                best = rank;
                found = static_cast<int>(mine - owned_.data());
                *match = this_match;
            }
        }
        return found;
    }

    // A, or Quick join: the room's password first when it wants one we do not have, then the test of the room, then it is the room in use.
    void Use(std::size_t idx, bool asked_again) {
        if (idx >= entries_.size()) {
            return;
        }
        const Entry e = entries_[idx];
        const Probe p = probes_[idx];
        if (lobbies::IsFull(p)) {
            g.ShowToast("This room is full: try another one.", col::kWarn, 3500);
            return;
        }
        if ((p.lock == lobbies::Lock::Password && e.room.secret.empty()) || asked_again) {
            std::string password = asked_again ? std::string() : e.room.secret;
            if (!g.EditText(asked_again ? "That password was wrong: the room's password" : "This room has a password: type it", password,
                            static_cast<int>(lobbies::kMaxSecretChars), false)) {
                return;
            }
            lobbies::SetLocal(local_, e.room, e.own_name ? e.room.name : std::string(), password == e.list_secret ? std::string() : password);
            SaveLocal();
            entries_[idx].room.secret = password;
        }
        setup::Server server;
        server.name = lobbies::SavedRoomName(entries_[idx].room.name, setup::kMaxServerNameChars);
        server.host = e.room.host;
        server.port = e.room.port;
        // (an open room is asked with no password: one the user kept from before would be refused)
        server.password = (p.lock == lobbies::Lock::Open && !asked_again) ? std::string() : entries_[idx].room.secret;
        const std::string key = lobbies::RoomKey(e.room);
        PushPage(pages::NewProbePage(server, [this, server, key, asked_again](pages::ProbeOutcome outcome) {
            const int idx_now = IndexOf(key);
            if (outcome == pages::ProbeOutcome::Reached) {
                Select(server, idx_now, true);
            } else if (outcome == pages::ProbeOutcome::WrongPassword && idx_now >= 0 && !asked_again) {
                Use(static_cast<std::size_t>(idx_now), true);
            } else if (outcome == pages::ProbeOutcome::Failed || outcome == pages::ProbeOutcome::WrongPassword) {
                PushPage(pages::NewConfirmPage("Connection test failed", "The room did not let this console in. Use it as the active room anyway?",
                                               "Yes, use it", "No", [this, server, idx_now](bool yes) {
                                                   if (yes) {
                                                       Select(server, idx_now, false);
                                                   }
                                               }));
            } else {
                g.ShowToast("Test cancelled: the room in use did not change.", col::kText, 3500);
            }
        }));
    }

    int IndexOf(const std::string& key) const {
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (lobbies::RoomKey(entries_[i].room) == key) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // The room in use from now on (AzaharLink.ini only: the saved rooms do not change). Then, when somebody in it plays a title of this console
    // that does not have the plugin yet, the question to enable it.
    void Select(const setup::Server& server, int idx, bool reached) {
        g.cfg.host = server.host;
        g.cfg.port = server.port;
        g.cfg.password = server.password;
        g.Save();
        const std::string name = idx >= 0 ? entries_[static_cast<std::size_t>(idx)].room.name : server.name;
        g.public_room_key = ActiveKey();
        g.public_room_name = name;
        if (g.save_ok) {
            g.ShowToast("Room: " + gfx::Clean(name), col::kOn);
        }
        rows_dirty_ = true;
        if (idx < 0) {
            return;
        }
        if (!server.password.empty() && reached) {
            // (the room took the password: its padlock says so at once, without waiting for the next pass)
            Probe& took = probes_[static_cast<std::size_t>(idx)];
            took.lock = lobbies::Lock::Password;
            took.secret_ok = true;
            RememberSeen(lobbies::RoomKey(entries_[static_cast<std::size_t>(idx)].room), took);
        }
        // (the game the most players play first: MemberOrder puts the biggest group first, the user's own game first on a tie)
        const Probe& p = probes_[static_cast<std::size_t>(idx)];
        for (std::size_t mi : lobbies::MemberOrder(p, owned_, titles_)) {
            const lobbies::Owned* mine = nullptr;
            const lobbies::Match match = lobbies::MatchOf(p.members[mi], owned_, titles_, &mine);
            if ((match != lobbies::Match::Same && match != lobbies::Match::Family) || !mine) {
                continue;
            }
            for (const auto& game : titles::Scan().games()) {
                if (lobbies::TitleKey(game->tid) == mine->key && setup::ReadSlot(game->tid).slot == setup::Slot::Off) {
                    const u64 tid = game->tid;
                    const std::string game_name = gfx::Clean(game->name);
                    PushPage(pages::NewConfirmPage("Enable the game?",
                                                   game_name + " is played in this room, and 3DS Link is not enabled for it yet. Enable it now?",
                                                   "Yes, enable it", "Not now", [this, tid, game_name](bool yes) {
                                                       if (yes) {
                                                           EnableNow(tid, game_name);
                                                       }
                                                   }));
                    return;
                }
            }
        }
    }

    // "Yes, enable it": the plugin goes into the title's folder now (the worker of Enable Titles), and a message says when it is there. When the
    // plugin file on the card is not the app's (missing, another version) the copy cannot be made: Enable Titles opens, it says why and puts the
    // file back.
    void EnableNow(u64 tid, const std::string& game_name) {
        g.LookAtPlugin(false);
        bool exists = false;
        setup::DescribePlugin(g.paths.plugin_file, &exists);
        if (!exists || g.plugin_card != setup::PluginCard::Same) {
            PushPage(pages::NewEnableGamePage());
            return;
        }
        setup::CopyWorker& worker = setup::CopyWorker::Get();
        if (!worker.Pending(tid, nullptr)) {
            worker.Submit(tid, setup::CopyKind::Enable, g.paths.plugin_file);
        }
        PushPage(new EnablingPage(tid, game_name)); // (nothing can be pressed until the copy is over)
    }

    void QuickJoin() {
        if (!owned_ready_) {
            g.ShowToast("Your titles are still being read: try again in a moment.", col::kWarn, 3500);
            return;
        }
        const int pick = lobbies::QuickPick(entries_, probes_, owned_, titles_);
        if (pick < 0) {
            g.ShowToast(scanning_ ? "Still asking the rooms: try again in a moment." : "No room with a free slot plays one of your games right now.",
                        col::kWarn, 4000);
            return;
        }
        Use(static_cast<std::size_t>(pick), false);
    }

    // X: the room among the favourites, the saved rooms of the Server list (one of the 10), at once: its name can be changed there (only its
    // name: the rest comes with the list). On a favourite X takes it away.
    void Keep(std::size_t idx) {
        if (idx >= entries_.size()) {
            return;
        }
        const Entry e = entries_[idx];
        setup::Server server;
        server.name = lobbies::SavedRoomName(e.room.name, setup::kMaxServerNameChars);
        server.host = e.room.host;
        server.port = e.room.port;
        server.password = probes_[idx].lock == lobbies::Lock::Open ? std::string() : e.room.secret; // (an open room is kept with no password)
        const int already = g.servers.Find(server);
        if (already >= 0) {
            // (X again takes it away: a favourite added by mistake goes as quickly as it came)
            g.servers.Remove(static_cast<std::size_t>(already));
            if (g.SaveServers()) {
                g.ShowToast("Removed from your favourites (the Server list).", col::kText, 3000);
            }
            Rebuild();
            return;
        }
        if (g.servers.full() || g.servers.Add(server) == setup::ServerList::AddResult::Full) {
            g.ShowToast("10 rooms in the Server list: remove one first (X there).", col::kWarn, 4000);
            return;
        }
        if (g.SaveServers()) {
            g.ShowToast(Fmt("Added to your favourites: the Server list (%u of 10).", static_cast<unsigned>(g.servers.size())), col::kOn, 3500);
        }
        Rebuild();
    }

    lobbies::List lobby_list_;
    lobbies::Local local_;
    const lobbies::Titles& titles_ = TitlesCache().titles;
    std::vector<Entry> entries_;
    std::vector<Probe> probes_;
    std::vector<bool> fresh_; // the answer is from this page's pass (else from an earlier one, shown faint)
    std::vector<bool> saved_; // the room is among the saved ones too
    std::vector<std::size_t> order_;
    std::vector<lobbies::Owned> owned_;
    std::vector<const u16*> owned_icons_; // (the icon of each of owned_, from the scan of the titles)
    std::vector<std::string> owned_names_; // (the name of each of owned_)
    bool owned_ready_ = false;
    bool local_ok_ = true;
    bool rescan_ = false;     // a new list came during a pass: the rooms are asked again when it ends
    std::string gone_;        // the room in use left the list: the message comes at the first frame
    int scan_generation_ = 0; // the scan of the titles owned_ and owned_icons_ come from
    std::string cursor_key_;
    u64 scan_started_ms_ = 0;
    lobbies::Kind kind_ = lobbies::Kind::All;
    int region_ = 0;
    std::string search_;
    bool dirty_ = true, force_ = true, rows_dirty_ = true;
    u64 built_ms_ = 0;
    bool scanning_ = false, scan_wanted_ = false;
    u32 scan_gen_ = 0;
    u64 scan_try_ms_ = 0;
    bool lists_started_ = false, lists_handled_ = false, lists_failed_ = false;
    u64 lists_try_ms_ = 0;

};

void LobbiesPage::DrawTop() {
    DrawTitleBarStatus(gfx::kTopW, Title());
    // (no warning here: it is on the row of the Server list that opens this page; the whole screen is the room under the cursor)

    // The room under the cursor, on a card from the bar to the buttons.
    constexpr float kCardY = 30.0f, kCardH = 142.0f;
    constexpr float kLastLine = kCardY + kCardH - 10.0f; // (the middle of the last line that fits in the card)
    constexpr float kLine = 16.0f;
    DrawCard(8, kCardY, 384, kCardH);
    const Row* row = Selected();
    if (row && row->id >= kRoomBase) {
        const std::size_t idx = static_cast<std::size_t>(row->id - kRoomBase);
        const Entry& e = entries_[idx];
        const Probe& p = probes_[idx];
        const std::string title = p.name.empty() ? e.room.name : p.name;
        // Whether the user has the game played in the room: its icon at the top right of the card, the words under the room's lines.
        lobbies::Match match = lobbies::Match::None;
        const int mine = RoomOwned(p, &match);
        const std::size_t at = static_cast<std::size_t>(mine);
        const bool icon = mine >= 0 && at < owned_icons_.size() && owned_icons_[at];
        if (icon) {
            gfx::DrawIcon(owned_icons_[at], setup::kLargeIcon, 360.0f, kCardY + 7.0f, 0.5f);
        }
        const float top_w = icon ? 334.0f : 364.0f; // (the name and the address stop before the icon)
        gfx::DrawFitted(18, kCardY + 13, 0.6f, 0.44f, top_w, col::kText, gfx::Clean(title));
        std::string line = Fmt("%s:%u", e.room.host.c_str(), static_cast<unsigned>(e.room.port));
        if (p.lock == lobbies::Lock::Password) {
            line += e.room.secret.empty() ? "  -  password: needed" : (p.secret_ok ? "  -  password: right" : "  -  password: kept");
        }
        if (e.kept) {
            line += "  -  no longer in the list";
        }
        if (p.version != 0 && p.version != azl::room::kNetworkVersion) {
            line += Fmt("  -  another Azahar (version %u)", static_cast<unsigned>(p.version));
        }
        gfx::DrawFitted(18, kCardY + 31, 0.46f, 0.36f, top_w, col::kDim, gfx::Clean(line));
        float y = kCardY + 31 + kLine;
        if (!p.description.empty()) {
            // (two lines at most, the end cut with "...": a long description shrunk on one line could not be read)
            const float lh = gfx::LineHeight(0.42f);
            const float used = gfx::DrawWrapped(18, y - lh * 0.5f, 364, 0.42f, col::kDim, gfx::Clean(p.description), 2);
            y += used - lh + kLine;
        }
        if (mine >= 0) {
            // (two lines at most, in a type that can be read: "of another region (EUR): Monster Hunter 3 Ultimate" is long)
            const std::string name = at < owned_names_.size() ? gfx::Clean(owned_names_[at]) : std::string();
            const lobbies::Title* own_title = owned_[at].title;
            const std::string text = match == lobbies::Match::OtherRegion
                                         ? "You have this game, of another region (" +
                                               std::string(own_title ? lobbies::RegionName(own_title->region) : "?") + "): " + name
                                         : "You have this game installed: " + name;
            const float lh = gfx::LineHeight(0.46f);
            const float used = gfx::DrawWrapped(18, y - lh * 0.5f, 364, 0.46f, match == lobbies::Match::OtherRegion ? col::kWarn : col::kOn, text, 2);
            y += used - lh + kLine + 2.0f;
        }
        if (p.state == lobbies::State::Offline) {
            gfx::DrawText(18, y, 0.48f, col::kOff, "The room did not answer.");
        } else if (p.has_info && p.members.empty()) {
            gfx::DrawText(18, y, 0.48f, col::kDim, "Nobody is in the room now.");
        } else if (p.has_info) {
            const std::vector<std::size_t> order = lobbies::MemberOrder(p, owned_, titles_);
            int shown = 0;
            for (std::size_t mi : order) {
                const int left = static_cast<int>(order.size()) - shown;
                if (y > kLastLine || (left > 1 && y + kLine > kLastLine)) {
                    break; // (the last line that fits says how many more there are)
                }
                const lobbies::Member& m = p.members[mi];
                std::string text = gfx::Clean(m.nickname) + "  ";
                u32 color = col::kText;
                if (m.game_id == 0) {
                    text += "(not playing)";
                    color = col::kDim;
                } else {
                    const lobbies::Title* t = titles_.Find(m.game_id);
                    text += "[" + std::string(t ? lobbies::RegionName(t->region) : "?") + "] " + gfx::Clean(m.game);
                    const lobbies::Owned* owned = nullptr;
                    const lobbies::Match member_match = lobbies::MatchOf(m, owned_, titles_, &owned);
                    if (member_match == lobbies::Match::Same || member_match == lobbies::Match::Family) {
                        color = col::kOn;
                    } else if (member_match == lobbies::Match::OtherRegion) {
                        color = col::kWarn;
                    }
                }
                gfx::DrawFitted(18, y, 0.48f, 0.36f, 364, color, text);
                y += kLine;
                ++shown;
            }
            if (static_cast<int>(order.size()) > shown) {
                gfx::DrawText(18, y, 0.44f, col::kDim, Fmt("+%d more", static_cast<int>(order.size()) - shown));
            }
        } else if (p.state == lobbies::State::Online) {
            gfx::DrawText(18, y, 0.48f, col::kDim, "The room answered, but did not say who is in it.");
        } else {
            gfx::DrawText(18, y, 0.48f, col::kDim, "Asking the room...");
        }
    } else if (row) {
        gfx::FillRect(8, kCardY, 3, kCardH, col::kAccent);
        gfx::DrawText(18, kCardY + 14, 0.58f, col::kAccent, gfx::Ellipsize(row->note_title.empty() ? row->label : row->note_title, 0.58f, 364));
        gfx::DrawWrapped(18, kCardY + 32, 364, 0.48f, col::kText, row->note, 6);
    }

    // The keys of the pad, as the buttons of the footer: what each one does now.
    const bool on_room = row && row->id >= kRoomBase;
    const bool on_saved = on_room && saved_[static_cast<std::size_t>(row->id - kRoomBase)]; // (X takes a favourite away)
    constexpr int kKeys = 5;
    constexpr float kGap = 4.0f;
    const float w = (384.0f - kGap * static_cast<float>(kKeys - 1)) / static_cast<float>(kKeys);
    // (the search on its button: cut with "..." where the button ends, in the button's own type; shrunk, it could not be read and the key went)
    const float search_w = w - 8.0f - (gfx::TextWidth("Y", 0.34f) + 10.0f) - 6.0f;
    const std::string search = search_.empty() ? std::string("Search") : gfx::Ellipsize(gfx::Clean(search_), 0.42f, search_w);
    struct TopKey {
        const char* key;
        std::string label;
        bool enabled;
    };
    const TopKey keys[] = {
        {"L", lobbies::KindName(kind_), true}, // (L at the left, R at the right, as on the console)
        {"R", lobbies::RegionFilterName(lobbies::kRegionFilters[region_]), true},
        {"Y", search, true},
        {"X", on_saved ? "Unfavourite" : "Favourite", on_room},
        {"SELECT", "Refresh", !scanning_},
    };
    for (int i = 0; i < kKeys; ++i) {
        const Rect r{8.0f + static_cast<float>(i) * (w + kGap), 177.0f, w, 22.0f};
        DrawButton(r, keys[i].key, keys[i].label.c_str(), keys[i].enabled, false, true);
    }
    // What the small squares of the rows say.
    DrawLobbyLegend(12, 210, 376);

    // How many rooms answered and how many play, the date of the list, and the pass that runs: at the bottom.
    int online = 0, players = 0, answered = 0;
    for (std::size_t i = 0; i < probes_.size(); ++i) {
        const Probe& p = probes_[i];
        online += p.state == lobbies::State::Online ? 1 : 0;
        players += static_cast<int>(p.members.size());
        answered += fresh_[i] ? 1 : 0;
    }
    std::string summary = Fmt("%d room%s online, %d player%s", online, online == 1 ? "" : "s", players, players == 1 ? "" : "s");
    if (!lobby_list_.updated.empty()) {
        summary += "  -  list of " + lobby_list_.updated;
    }
    if (lists_failed_) {
        summary += " (not updated)";
    }
    if (g.ToastActive()) {
        // (the page has no status strip: the last message, "Added to your favourites" ..., takes the place of the count while it lasts)
        gfx::DrawFitted(12, 227, 0.44f, 0.34f, scanning_ ? 284.0f : 376.0f, g.toast_color, g.toast);
    } else {
        gfx::DrawFitted(12, 227, 0.44f, 0.34f, scanning_ ? 284.0f : 376.0f, col::kDim, summary);
    }
    if (scanning_ && !entries_.empty()) {
        // (a shorter bar: the line keeps room for 999 rooms and 9999 players while the rooms are asked)
        DrawProgressBar(302, 223, 86, 8, static_cast<float>(answered) / static_cast<float>(entries_.size()));
    }
    DrawFrameBorder(gfx::kTopW);
}

} // namespace

ui::Page* NewLobbiesPage() {
    return new LobbiesPage;
}

bool InPublicList(const std::string& host, u16 port) {
    for (const lobbies::Room& room : ReadList().rooms) {
        if (lobbies::SameRoom(room.host, room.port, host, port)) {
            return true;
        }
    }
    return false;
}

bool RefreshPublicRoomName() {
    g.public_room_key.clear();
    g.public_room_name.clear();
    if (g.cfg.host.empty() || ActiveIsSaved()) {
        return false;
    }
    const lobbies::List list = ReadList();
    const lobbies::Local local = ReadLocal();
    for (const Entry& e : lobbies::Entries(list, local)) {
        if (lobbies::SameRoom(e.room.host, e.room.port, g.cfg.host, g.cfg.port)) {
            g.public_room_key = ActiveKey();
            g.public_room_name = e.room.name;
            return true;
        }
    }
    return false;
}

} // namespace pages
