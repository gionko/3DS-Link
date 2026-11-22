// Page: Download Play > Join lobby - the console joins a Download Play host of the room from 3DS Link (1.1.4, route B, piece B4:
// analysis/piano-1.1.4-2026-11-15/specifiche/8-INGRESSO-DA-3DSLINK.md sections 1, 3.5 and D-B3, approved by the user on 2026-11-16; under the page
// Download Play since spec 9, whose Enable games prepares the children: every text that says where a child comes from names Download Play > Enable
// games). Since the console test of 2026-11-18 "Download Play" is said once, in the top bar: the bar of the touch screen says how the search goes
// ("Looking for hosts...", "N hosts found"), a host's row is its game, and who hosts it, the room and the players are on the top screen.
//   - The gate first (dlpjoin::JoinGate): the installed app, Luma3DS and its Plugin Loader, 3DS Link on, a room the child joins by itself, the HOME
//     Menu, no work under way, the worker of the children idle, a child ready. A refusal is a card with its sentence and, when there is one, the
//     page that puts it right; nothing starts.
//   - The list: the session (dlp_join.hpp) joins the room of the settings and looks for hosts. A host whose child is Ready here, in the index file
//     the plugin reads and not full is selectable; the others are grey with their reason; a host of a game with no Download Play guest here is not
//     listed, only counted; the members that play a game ready here and have not opened its Download Play yet are grey too. SELECT looks again.
//   - A: the gate again (the work, the worker, the children of now), HOME and sleep off (dlp::WorkHolds) and the join. The waiting screen says the
//     phase; B in the join, in the lobby or after START leaves the host and goes back to the list, in the room. The host gone: "<host> left".
//   - The passphrase: one frame "Starting <game>...", then Session::HandOverAndLeave (the 16 bytes to ns:s, the room left politely) and, only when
//     ns:s answered 0 and the title is one of ours, g.child_jump: main.cpp chain-loads the hidden prepared child when the app ends. A refusal of
//     ns:s is a card: no jump, the app stays.
// Privacy: the hosts' MACs are only handed to AskJoin; nothing here writes a log or a file. The names, icons and sizes of the children are copied
// from their blocks into the page (the icons' textures are keyed by where the data is: the page's own copies live as long as the page, and
// every icon is drawn from them - never from a second copy, whose buffer a later join would refill under the same address).
#include <cstring>
#include <string>
#include <vector>

#include "copyworker.hpp"
#include "dlp_guest.hpp"
#include "dlp_guest_store.hpp"
#include "dlp_join.hpp"
#include "dlp_prepare.hpp"
#include "lobby_scan.hpp"
#include "pages.hpp"
#include "system.hpp"
#include "titles.hpp"
#include "update_worker.hpp"

using namespace ui;

namespace {

constexpr const char* kTitle = "Download Play > Join lobby";
constexpr const char* kShortTitle = "Download Play";
constexpr std::size_t kIconUnits = static_cast<std::size_t>(setup::kLargeIcon) * setup::kLargeIcon;
static_assert(kIconUnits * 2 == azl::dlp_children::kBlockTransferAt - azl::dlp_children::kBlockIconAt, "a block's icon is the big icon of the SMDH");

// UTF-16 little endian (at most `units`, ended by a zero) to UTF-8; control characters become spaces, a lone surrogate U+FFFD.
std::string Utf8Of(const u8* p, std::size_t units) {
    std::string out;
    for (std::size_t i = 0; i < units; ++i) {
        u32 cp = static_cast<u32>(p[2 * i]) | (static_cast<u32>(p[2 * i + 1]) << 8);
        if (cp == 0) {
            break;
        }
        if (cp >= 0xD800 && cp < 0xDC00) {
            const u32 lo = i + 1 < units ? static_cast<u32>(p[2 * i + 2]) | (static_cast<u32>(p[2 * i + 3]) << 8) : 0;
            if (lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xDC00 && cp < 0xE000) {
            cp = 0xFFFD;
        }
        if (cp < 0x20 || cp == 0x7F) {
            cp = ' ';
        }
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// The player name a lobby announces (UTF-16 in the console's order: little endian).
std::string HostName(const azl::Group& group) {
    u8 bytes[sizeof(group.name)];
    std::memcpy(bytes, group.name, sizeof(bytes));
    const std::string name = Utf8Of(bytes, azl::kNameUnits);
    return name.empty() ? std::string("(no name)") : name;
}

bool UpdateBusy() {
    const update::Stage s = update::Worker::Get().Read().stage;
    return s == update::Stage::Checking || s == update::Stage::Downloading || s == update::Stage::Installing || update::Worker::Get().ListsRunning();
}

// What stops the page now (spec 8 section 3.2). `guests_ready`: the children Ready (the table's at A; before the session, the app's Installed records).
// `entry`: the gate of the entrance (the review of the build 2h, U3): only the work the session's start waits for (dlpjoin::EntryWork).
dlpjoin::Facts FactsNow(u32 guests_ready, bool entry = false) {
    dlpjoin::Facts f = {};
    f.installed_app = !envIsHomebrew();
    f.luma_ok = !g.luma_too_old;
    f.loader_on = g.loader == setup::LoaderState::Enabled;
    f.plugin_on = g.cfg.enabled && g.cfg.tunnel;
    f.room_set = !g.cfg.host.empty();
    f.autoconnect = g.cfg.autoconnect;
    f.menu_off = !setup::MenuOn(g.cfg);
    bool home_menu = false;
    f.home_menu = R_SUCCEEDED(APT_IsRegistered(aptGetMenuAppID(), &home_menu)) && home_menu;
    dlpjoin::Work w = {};
    w.copies = setup::CopyWorker::Get().PendingCount() > 0;
    w.titles_scan = titles::Scan().started() && !titles::Scan().done();
    w.update = UpdateBusy();
    w.lobbies_scan = lobbies::Scanner::Get().running();
    f.work_running = entry ? dlpjoin::EntryWork(w) : dlpjoin::AnyWork(w);
    f.guest_worker = dlpguest::Running();
    f.delivery_up = false;  // (Titles Delivery's wireless is opened in its own pages only, never under this one: the page Download Play opens this page)
    f.guests_ready = guests_ready;
    return f;
}

// The children the app's records say are whole and ours: the gate's count before the session reads the index file (the session counts them again).
// (DPAPP-2j) never a child of the system form of the build 2h: the session takes it for "not prepared" (dlp_join.cpp), so the gate does too
u32 InstalledGuests() {
    u32 n = 0;
    for (const dlpguest::Guest& guest : dlpguest::LoadGuests()) {
        n += guest.state == dlpguest::GuestState::Installed && dlpguest::IsOwnedGuestTid(guest.prepared) && dlpguest::Jumpable(guest.form) ? 1u : 0u;
    }
    return n;
}

// (the review of the build 2h; DPAPP-2j) the games whose guest a test build made (the system form of the build 2h: never joined, never jumped to):
// "to update" in Enable games
std::string TestBuildGuests() {
    std::string names;
    for (const dlpguest::Guest& guest : dlpguest::LoadGuests()) {
        if (guest.state == dlpguest::GuestState::Installed && dlpguest::IsOwnedGuestTid(guest.prepared) && !dlpguest::Jumpable(guest.form)) {
            names += (names.empty() ? "" : ", ") + dlpguest::GameName(guest.game);
        }
    }
    return names;
}

// A refusal of the gate: its sentence, and the page that puts it right when there is one.
void ShowRefusal(dlpjoin::Why why) {
    const std::string text = dlpjoin::WhyText(why);
    switch (why) {
    case dlpjoin::Why::PluginOff:
    case dlpjoin::Why::NoAutoConnect:
        PushPage(pages::NewConfirmPage(kShortTitle, text, "Settings", "Back", [](bool yes) {
            if (yes) {
                PushPage(pages::NewConfigPage());
            }
        }));
        break;
    case dlpjoin::Why::LumaTooOld:
    case dlpjoin::Why::LoaderOff:
        PushPage(pages::NewConfirmPage(kShortTitle, text, "Check", "Back", [](bool yes) {
            if (yes) {
                PushPage(pages::NewInstallCheckPage());
            }
        }));
        break;
    case dlpjoin::Why::NoRoom:
        PushPage(pages::NewConfirmPage(kShortTitle, text, "Rooms", "Back", [](bool yes) {
            if (yes) {
                PushPage(pages::NewServersPage());
            }
        }));
        break;
    case dlpjoin::Why::NoGuests: {
        // (the review of the build 2h) a guest made by a test build is not "no game": it is one to update
        const std::string test_built = TestBuildGuests();
        const std::string said = test_built.empty() ? text : test_built + ": made by a test build, its guest must be updated in Download Play > Enable games";
        PushPage(pages::NewConfirmPage(kShortTitle, said, "Enable games", "Back", [](bool yes) {
            if (yes) {
                PushPage(pages::NewDlpGamesPage()); // (over the page Download Play: B comes back to it)
            }
        }));
        break;
    }
    default:
        PushPage(pages::NewMessagePage(kShortTitle, text));
        break;
    }
}

std::string RoomErrorText(azl::RoomClient::Error e) {
    using E = azl::RoomClient::Error;
    switch (e) {
    case E::CouldNotConnect: return "No answer from the room: check the WiFi and the room's address.";
    case E::NameCollision: return "Another player in the room has this console's name.";
    case E::MacCollision:
    case E::ConsoleIdCollision: return "This console is still in the room (a game with 3DS Link may be connected): wait a moment.";
    case E::WrongVersion: return "The room speaks another protocol version.";
    case E::WrongPassword: return "The room's password is wrong or missing (Azahar Emulator Room, Y).";
    case E::RoomIsFull: return "The room is full.";
    case E::HostBanned: return "The room's host banned this console.";
    case E::HostKicked: return "The room's host removed this console.";
    case E::LostConnection: return "The connection to the room was lost.";
    case E::RoomClosed: return "The room was closed.";
    case E::NetworkUnavailable: return "The console could not open a network socket: is the WiFi connected?";
    default: return "The room did not accept the connection.";
    }
}

// What a grey row says at its right, and on the top screen.
const char* GreyValue(const dlpjoin::Host& h) {
    using V = azl::dlp_children::Verdict;
    if (!h.index_row) {
        return "index not written";
    }
    if (h.v == V::Ready && h.full) {
        return "full";
    }
    switch (h.v) {
    case V::OtherVersion: return "another version";
    case V::NotPrepared: return "not prepared";
    case V::NoChildPlugin: return "no plugin";
    case V::NoGame: return "no game";
    default: return "not known yet";
    }
}

const char* GreyNote(const dlpjoin::Host& h) {
    using V = azl::dlp_children::Verdict;
    if (!h.index_row) {
        return "Its Download Play guest is not in the list the plugin reads (the file could not be written): start 3DS Link again so that "
               "it writes it.";
    }
    if (h.v == V::Ready && h.full) {
        return "The host's lobby is full.";
    }
    switch (h.v) {
    case V::OtherVersion: return "The host's game is another version than yours: both consoles need the same update of the game.";
    case V::NotPrepared: return "Its Download Play guest is not ready on this console: enable the game in Download Play > Enable games.";
    case V::NoChildPlugin: return "The plugin is missing from its Download Play guest: disable the game and enable it again in Download Play > Enable games.";
    case V::NoGame: return "This game is not on this console.";
    default: return "3DS Link has not read what the host announces yet.";
    }
}

class DlpJoinPage : public ListPage {
public:
    ~DlpJoinPage() override {
        holds_.Give();
        session_.Stop();      // (the polite leave of D-B10: the room's queue first, then the room)
        gfx::ReleaseIcons();  // (the textures of the page's icons: their data goes with the page)
    }

    void Update(const Input& in) override {
        if (!begun_) {
            begun_ = true;
            Begin();
            return;
        }
        switch (mode_) {
        case Mode::Waiting:
            // (the build 2h, U3) the work under way that is not this page's: shown with its bar until it ends, then the gate again and the session -
            // by itself. B goes back. (the review of the build 2h) At the entrance only the work the session's start waits for (EntryWork); at A
            // every job the way out waits for, and then the list again: A joins the host (the list may have changed meanwhile)
            if (wait_ == Wait::Join && session_.phase() == dlpjoin::Phase::RoomLost) {
                mode_ = Mode::RoomLost;
                return;
            }
            if ((in.down & KEY_B) || Tapped(in, LayoutFooter(ModeFooter()).back)) {
                if (wait_ == Wait::Join) {
                    mode_ = Mode::List;
                } else {
                    PopPage();
                }
                return;
            }
            if (++waited_frames_ % 15 == 0) {
                const dlpjoin::Facts f = FactsNow(1, wait_ == Wait::Entry); // (only the work: the records are read when it is over, in Begin)
                if (!f.work_running && !f.guest_worker) {
                    if (wait_ == Wait::Join) {
                        mode_ = Mode::List;
                        g.ShowToast("The work is over: A joins the host", col::kDim, 3500);
                    } else {
                        Begin();
                    }
                }
            }
            return;
        case Mode::List:
            if (session_.phase() == dlpjoin::Phase::RoomLost) {
                mode_ = Mode::RoomLost;
                return;
            }
            ListPage::Update(in);
            return;
        case Mode::RoomLost:
            if ((in.down & KEY_A) || Tapped(in, LayoutFooter(ModeFooter()).action)) {
                Restart();
            } else if ((in.down & KEY_B) || Tapped(in, LayoutFooter(ModeFooter()).back)) {
                OnBack();
            }
            return;
        case Mode::Session:
            UpdateSession(in);
            return;
        case Mode::Handing:
            HandOver();
            return;
        }
    }

    void DrawTop() override {
        DrawTitleBarStatus(gfx::kTopW, kTitle);
        if (mode_ == Mode::Waiting) {
            // (the build 2h, U3: which work, and how far it is - as Enable Titles and Enable games show their own; the strip says the room)
            std::string title, text, detail;
            float fraction = -1.0f;
            if (!pages::WorkUnderWay(title, text, detail, fraction)) {
                title = "Please wait...";
                text = "3DS Link finishes the work under way.";
            }
            DrawCard(10, 32, 380, 168);
            gfx::FillRect(10, 32, 3, 168, col::kAccent);
            gfx::DrawText(26, 48, 0.55f, col::kAccent, gfx::Ellipsize(title, 0.55f, 350));
            gfx::DrawWrapped(26, 66, 350, 0.45f, col::kText, text, 3);
            if (fraction >= 0.0f) {
                DrawProgressBar(26, 118, 348, 14, fraction);
            }
            gfx::DrawWrapped(26, 142, 350, 0.40f, col::kDim, detail, 4); // (the review of the build 2h: what follows is said once, below)
            DrawStatusStrip(10, 206, 380, 28);
            DrawFrameBorder(gfx::kTopW);
            return;
        }
        if (mode_ == Mode::Session || mode_ == Mode::Handing) {
            DrawChosen(PhaseText());
        } else if (mode_ == Mode::RoomLost) {
            DrawCard(10, 32, 380, 168);
            gfx::DrawText(26, 52, 0.55f, col::kOff, "Not in the room");
            gfx::DrawWrapped(26, 70, 350, 0.46f, col::kText, RoomErrorText(session_.room_error()), 4);
            gfx::DrawWrapped(26, 140, 350, 0.42f, col::kDim, "A tries again, B goes back.", 2);
        } else if (const Row* row = Selected()) {
            const int host = row->id - kHostBase;
            if (host >= 0 && host < static_cast<int>(hosts_.size())) {
                DrawHost(hosts_[static_cast<std::size_t>(host)]);
            } else if (row->id == kSearch) {
                DrawSearch(*row);
            } else {
                DrawNote(10, 32, 380, 168, *row);
            }
        }
        if (g.ToastActive()) {
            // (the word of the moment - "<host> left", a join refused, SELECT - under the cards, as Titles Delivery shows it: this page drew none, so a
            // refused join came back to the list without a word - the review of 2026-11-18. Only then: the cards already say the room)
            DrawStatusStrip(10, 206, 380, 28);
        }
        DrawFrameBorder(gfx::kTopW);
    }

    // (DPAPP-2h-sec) the footer of the modes that are not the list, one for the drawing and the touch: no START, so its buttons share the whole width
    // (kBackButton and ActionButton are the places of a footer WITH START - a touch in the middle of "B Back" did nothing)
    Footer ModeFooter() const {
        Footer footer;
        footer.start = false;
        if (mode_ == Mode::Waiting) {
            footer.back_label = "Back";
        } else if (mode_ == Mode::RoomLost) {
            footer.action_label = "Try again";
        } else {
            footer.back = Leavable();
            footer.back_label = "Leave";
        }
        return footer;
    }

    void DrawBottom() override {
        if (mode_ == Mode::List) {
            ListPage::DrawBottom();
            return;
        }
        // (the game being joined, or the room that was lost: not "Download Play" again - 2026-11-18)
        const std::string bar = mode_ == Mode::Waiting ? std::string("Please wait") : (mode_ == Mode::RoomLost ? gfx::Clean("Room " + RoomLabel()) : chosen_game_);
        DrawTitleBar(gfx::kBottomW, gfx::Ellipsize(bar, 0.56f, 200).c_str(), std::string());
        constexpr float kH = 110.0f;
        // (the build 2h, U6) centred in the space between the title bar and the footer: the same black above and below
        const float y = kBarH + 1.0f + (kFooterTop - kBarH - 1.0f - kH) * 0.5f;
        DrawCard(8, y, 304, kH);
        gfx::FillRect(8, y, 3, kH, mode_ == Mode::RoomLost ? col::kOff : col::kAccent);
        if (mode_ == Mode::Waiting) {
            gfx::DrawText(160, y + 28, 0.55f, col::kText, "Waiting for 3DS Link", gfx::Align::Center);
            gfx::DrawWrapped(22, y + 44, 276, 0.42f, col::kDim,
                             wait_ == Wait::Join ? "When the work under way is over, A joins the host." : "Join lobby starts by itself when the work under way is over.", 3);
            DrawSpinner(y + kH - 18.0f);
        } else if (mode_ == Mode::RoomLost) {
            gfx::DrawText(160, y + 28, 0.55f, col::kText, "Not in the room", gfx::Align::Center);
            gfx::DrawWrapped(22, y + 44, 276, 0.42f, col::kDim, RoomErrorText(session_.room_error()), 3);
        } else {
            gfx::DrawText(160, y + 28, 0.55f, col::kText, gfx::Ellipsize(PhaseText(), 0.55f, 280), gfx::Align::Center);
            gfx::DrawWrapped(22, y + 44, 276, 0.42f, col::kDim,
                             Leavable() ? "HOME is off until the game starts. B leaves the host." : "Do not switch the console off.", 3);
            DrawSpinner(y + kH - 18.0f);
        }
        DrawFooterBackground();
        DrawFooter(ModeFooter());
        DrawFrameBorder(gfx::kBottomW);
    }

protected:
    const char* Title() const override {
        return kTitle;
    }
    // How the search goes, not "Download Play" again (the top bar says it): "Joining the room...", "Looking for hosts...", "N hosts found". Nothing at
    // its right: the room is on the top screen (a long name of a public room ran over the title - the review of 2026-11-18).
    const char* BarTitle() const override {
        return search_.c_str();
    }
    const char* Hint() const override {
        return "SELECT scan again";
    }
    const char* ActionLabel() const override {
        const Row* row = Selected();
        return row != nullptr && row->kind == Kind::Action ? "Join" : "";
    }
    bool StartButton() const override {
        return false;
    }
    void OnBack() override {
        holds_.Give();
        session_.Stop();
        PopPage();
    }
    void OnKeys(const Input& in) override {
        if ((in.down & KEY_SELECT) && session_.core() != nullptr) {
            session_.core()->AskScan();
            g.ShowToast("Looking for hosts again...", col::kDim, 1500);
        }
    }

    void BuildRows(std::vector<Row>& rows) override {
        rows.clear();
        hosts_.clear();
        found_ = 0;
        joinable_ = 0;
        search_ = "Looking for hosts...";
        if (session_.core() == nullptr) {
            return;
        }
        if (session_.phase() == dlpjoin::Phase::Joining) {
            search_ = "Joining the room...";
            Row r;
            r.id = kSearch;
            r.kind = Kind::Info;
            r.label = "Not in the room yet"; // (the bar says "Joining the room...", the card the room: each says it once - the review of 2026-11-18)
            r.busy = true;
            r.note = "3DS Link joins the room of the settings as \"Download Play (3DS Link)\". The game will join it with the same name when it starts.";
            rows.push_back(r);
            return;
        }
        int hidden = 0;
        std::size_t waiting_rows = 0;  // (the build 2f, B3: the members that wait, for the line of the scans)
        std::vector<std::string> host_names;
        const std::vector<dlpjoin::Host> all = session_.core()->Hosts();
        for (const dlpjoin::Host& h : all) {
            const dlpjoin::Show show = dlpjoin::ShowOf(h);
            if (show == dlpjoin::Show::Hidden) {
                ++hidden;
                continue;
            }
            const Child* c = ChildOf(h.g.child_tid_low);
            Row r;
            r.id = kHostBase + static_cast<int>(hosts_.size());
            r.kind = show == dlpjoin::Show::Join ? Kind::Action : Kind::Info;
            r.dim = show != dlpjoin::Show::Join;
            r.label = c != nullptr ? c->short_name : std::string("Another game");  // (an "index not written" child too; who hosts it: the top screen, DrawHost)
            joinable_ += show == dlpjoin::Show::Join ? 1 : 0;
            r.icon = c != nullptr && !c->icon.empty() ? c->icon.data() : nullptr;
            r.value = show == dlpjoin::Show::Join ? "ready" : GreyValue(h);
            r.value_color = show == dlpjoin::Show::Join ? col::kOn : col::kDim;
            r.chip = Fmt("%u/%u", static_cast<unsigned>(h.g.total_nodes), static_cast<unsigned>(h.g.max_nodes));
            host_names.push_back(HostName(h.g));
            hosts_.push_back(h);
            rows.push_back(r);
        }
        for (const dlpjoin::Member& m : session_.WaitingMembers(all)) {  // (less the members whose MAC hosts a lobby of the scan)
            bool hosting = false;
            for (const std::string& n : host_names) {
                hosting = hosting || n == m.nickname;
            }
            if (hosting) {
                continue;  // (its lobby is listed above: the room's nickname happens to be the beacon's name)
            }
            const Child* c = ChildByPrepared(azl::title_rules::PreparedOf(m.game_id));
            const std::string game = c != nullptr ? c->short_name : dlpguest::GameName(m.game_id);
            Row r;
            r.id = kMember;
            r.kind = Kind::Info;
            r.dim = true;
            r.label = game;  // (the player: the title of its note, on the top screen)
            r.icon = c != nullptr && !c->icon.empty() ? c->icon.data() : nullptr;
            r.value = "not open";
            r.value_color = col::kDim;
            r.note_title = gfx::Clean(m.nickname);
            r.note = "Plays " + game + ", no Download Play open yet. When the host opens it, the lobby appears here.";
            rows.push_back(r);
            ++waiting_rows;
        }
        if (session_.children() != nullptr) {
            // (the build 2f, B3) what this scan sees, one line of the log when it changed: in memory, written when the session ends (never a write here)
            session_.NoteScan(dlpjoin::ScanLine(all, *session_.children(), waiting_rows));
        }
        found_ = static_cast<int>(all.size());
        if (found_ > 0) {
            search_ = Fmt("%d host%s found", found_, found_ == 1 ? "" : "s");
        }
        if (rows.empty()) {
            Row r;
            r.id = kSearch;
            r.kind = Kind::Info;
            // (the bar says "Looking for hosts..." or "N hosts found": the row says what the list has - no lobby yet, or only hosts of games with no
            // guest here, counted below - and the card the room: never the same words three times, the review of 2026-11-18)
            r.label = found_ > 0 ? "No lobby you can join yet" : "No lobby yet";
            r.busy = true;
            r.note = "A host opens Download Play in its game (a console with 3DS Link, or Azahar) in this room. Only the games whose Download Play "
                     "guest is ready on this console can be joined. SELECT looks again.";
            rows.push_back(r);
        }
        if (hidden > 0) {
            Row r;
            r.id = kInfo;
            r.kind = Kind::Info;
            r.dim = true;
            r.gap = 6.0f;
            r.label = Fmt("%d other host%s", hidden, hidden == 1 ? "" : "s");
            r.value = "no guest here";
            r.value_color = col::kDim;
            r.note_title = r.label;
            r.note = "Games without a Download Play guest on this console: enable the game in Download Play > Enable games, and 3DS Link prepares its guest.";
            rows.push_back(r);
        }
    }

    void Activate(const Row& row, int step) override {
        const int host = row.id - kHostBase;
        if (step != 0 || row.kind != Kind::Action || host < 0 || host >= static_cast<int>(hosts_.size())) {
            return;
        }
        Join(hosts_[static_cast<std::size_t>(host)]);
    }

private:
    enum class Mode { List, RoomLost, Session, Handing, Waiting };
    /// (the review of the build 2h, U3) what Mode::Waiting waits for: the entrance (the update, the public lobbies, a guest: EntryWork), the
    /// entrance whose session found no child ready while copies or the scan ran (every job), A (every job the way out waits for, spec 8 3.2)
    enum class Wait { Entry, All, Join };
    void WaitFor(Wait why) {
        wait_ = why;
        waited_frames_ = 0;
        mode_ = Mode::Waiting;
    }
    enum Id { kInfo = 1, kMember, kSearch, kHostBase = 100 };
    // A child of the table as the page shows it: its names and icon, copied from its block (the table goes with the session).
    struct Child {
        u64 child = 0;
        u64 prepared = 0;
        std::string short_name;
        std::string long_name;
        std::vector<u16> icon;
    };

    // The first frame: the gate, then the session (the room, the table of the children). (the build 2h, U3) The work under way that is not this
    // page's - the scan, the copies, the update, the public lobbies, a guest - is waited for with its card and its bar (Mode::Waiting), never a
    // refusal with no word of what works: the gate stays the same (spec 8: nothing starts while it runs, and again at A).
    void Begin() {
        g.RefreshLoader();
        dlpjoin::Why why = dlpjoin::JoinGate(FactsNow(InstalledGuests(), true));
        if (why == dlpjoin::Why::WorkRunning || why == dlpjoin::Why::GuestWorker) {
            WaitFor(Wait::Entry);
            return;
        }
        if (why == dlpjoin::Why::Ok) {
            why = Start();
        }
        if (why == dlpjoin::Why::NoGuests && FactsNow(1).work_running) {
            WaitFor(Wait::All); // (the review of the build 2h: a copy of the plugin or the scan under way may be why no child is ready yet - waited for, said)
            return;
        }
        if (why != dlpjoin::Why::Ok) {
            PopPage();
            ShowRefusal(why);
        }
    }

    dlpjoin::Why Start() {
        dlpjoin::SessionIn in;
        in.host = g.cfg.host;
        in.port = g.cfg.port;
        in.password = g.cfg.password;
        in.nickname = g.EffectiveNickname();  // (the plugin's rule: the child comes back under the same name)
        in.mac = setup::ReadConsoleMac();     // (the child comes back with it)
        in.datadir = g.paths.datadir;
        const dlpjoin::Why why = session_.Start(FactsNow(InstalledGuests(), true), in);
        if (why == dlpjoin::Why::Ok) {
            CopyChildren();
            mode_ = Mode::List;
        }
        return why;
    }

    // A: the gate again (the work and the worker of now, the table's children), HOME and sleep off, the join.
    void Join(const dlpjoin::Host& h) {
        dlpjoin::Core* core = session_.core();
        if (core == nullptr || session_.children() == nullptr) {
            return;
        }
        const dlpjoin::Why why = dlpjoin::JoinGate(FactsNow(dlpjoin::ReadyCount(session_.children()->table)));
        if (why == dlpjoin::Why::WorkRunning || why == dlpjoin::Why::GuestWorker) {
            WaitFor(Wait::Join); // (the review of the build 2h, U3: the work the way out waits for, with its card and its bar - never a bare sentence)
            return;
        }
        if (why != dlpjoin::Why::Ok) {
            ShowRefusal(why);
            return;
        }
        const Child* c = ChildOf(h.g.child_tid_low);
        // (DPAPP-2j, R-A) the closed game pm may keep of this guest (the form M3c1: freed at the start of 3DS Link, zombie_free.cpp): still in memory,
        // the jump would start a second one (pm looks for 00040001|low, NS's, and does not see the closed 00040000|low) - refused, said; the
        // session's log says it (Session::ClosedGameLeft)
        bool unread = false;
        if (c != nullptr && c->prepared != 0 && session_.ClosedGameLeft(c->prepared, &unread)) {
            // (DPAPP-2j-rev) the list of the processes not read whole is no closed game: never "restart the console" for it
            g.ShowToast(unread ? std::string("3DS Link could not read which programs are in memory: press A to try again.")
                               : std::string(dlpguest::Phrase(dlpguest::kErrIdRunning)) + " " + dlpguest::Advice(dlpguest::kErrIdRunning),
                        col::kWarn, 6000);
            return;
        }
        chosen_name_ = HostName(h.g);
        chosen_game_ = c != nullptr ? (c->long_name.empty() ? c->short_name : c->long_name) : std::string("the game");
        chosen_child_ = h.g.child_tid_low;  // (its icon is drawn from children_: a copy here would keep the first join's address and texture)
        holds_.Take();
        refusals_at_ = core->refusals();
        rounds_at_ = core->Rounds();
        entered_ = false;
        core->AskJoin(h.g.host);
        mode_ = Mode::Session;
    }

    void UpdateSession(const Input& in) {
        dlpjoin::Core* core = session_.core();
        dlpjoin::Core::View seen;
        const dlpjoin::Phase p = session_.phase(&seen);  // (one look: the word, the join flag, the rounds and the refusals of the same Step)
        if (core == nullptr || p == dlpjoin::Phase::RoomLost) {
            holds_.Give();
            mode_ = Mode::RoomLost;
            return;
        }
        if (p == dlpjoin::Phase::Passphrase) {
            mode_ = Mode::Handing;  // (one frame says "Starting <game>..." before the hand-over)
            return;
        }
        if (p == dlpjoin::Phase::HostLeft) {
            core->AskLeave();
            BackToList(Fmt("%s left", chosen_name_.c_str()));
            return;
        }
        if (p == dlpjoin::Phase::Entering || p == dlpjoin::Phase::Waiting || p == dlpjoin::Phase::Starting) {
            entered_ = true;
            if ((in.down & KEY_B) || Tapped(in, LayoutFooter(ModeFooter()).back)) {
                core->AskLeave();  // (ClientStopSession, then the list again: the page stays in the room)
                BackToList(std::string());
            }
            return;
        }
        // the list again without B: the join was refused, or failed
        if (entered_) {
            BackToList(Fmt("%s did not let this console in (0x%08lX)", chosen_name_.c_str(), static_cast<unsigned long>(core->last_join_result())));
        } else if (seen.refusals != refusals_at_) {
            BackToList(Fmt("%s is no longer in the list", chosen_name_.c_str()));
        } else if (seen.rounds >= rounds_at_ + 2) {
            BackToList(Fmt("%s could not be joined (0x%08lX)", chosen_name_.c_str(), static_cast<unsigned long>(core->last_join_result())));
        }
    }

    void BackToList(const std::string& toast) {
        holds_.Give();
        mode_ = Mode::List;
        if (!toast.empty()) {
            g.ShowToast(gfx::Clean(toast), col::kWarn, 3500);
        }
    }

    // State 9: the 16 bytes to ns:s and the room left (Session::HandOverAndLeave), then the jump - only on ns:s's 0, only to a title of ours.
    void HandOver() {
        s32 rc = 0;
        const u64 prepared = session_.HandOverAndLeave(&rc);
        // (DPAPP-2j) never a child of the system form of the build 2h: main.cpp refuses it too (two guards)
        if (prepared != 0 && rc == 0 && dlpguest::IsOwnedGuestTid(prepared) && dlpguest::Jumpable(dlpguest::FormOf(prepared))) {
            g.child_jump = prepared;
            g.child_jump_left_ms = session_.left_room_ms();
            g.quit = true;  // (main.cpp: aptSetChainloader when the app ends; HOME stays off until then)
            return;
        }
        holds_.Give();
        PopPage();
        PushPage(pages::NewMessagePage(kShortTitle, rc == -1 ? std::string("The game did not start: the host's lobby had ended.")
                                                             : Fmt("Could not hand the game over (0x%08lX). The host goes on without this console.",
                                                                   static_cast<unsigned long>(rc))));
    }

    // A: the room again after it was lost.
    void Restart() {
        session_.Stop();
        rows_.clear();
        hosts_.clear();
        const dlpjoin::Why why = Start();
        if (why != dlpjoin::Why::Ok) {
            PopPage();
            ShowRefusal(why);
        }
    }

    // The names and icons of the table's children, copied once a session (the table goes with it).
    void CopyChildren() {
        const dlpjoin::Children* ch = session_.children();
        if (ch == nullptr) {
            return;
        }
        std::vector<Child> fresh;
        const azl::dlp_children::Table& t = ch->table;
        for (std::size_t r = 0; r < t.index.count && r < azl::dlp_children::kMaxRows; ++r) {
            Child c;
            c.child = t.index.rows[r].child;
            c.prepared = t.index.rows[r].prepared;
            if (azl::dlp_children::BlockStateOf(&t, r) == azl::dlp_children::BlockState::Read) {
                const u8* block = azl::dlp_children::BlockOf(&t, r);
                c.short_name = Utf8Of(block + azl::dlp_children::kBlockShortAt, 64);
                c.long_name = Utf8Of(block + azl::dlp_children::kBlockLongAt, 128);
                c.icon.resize(kIconUnits);
                std::memcpy(c.icon.data(), block + azl::dlp_children::kBlockIconAt, kIconUnits * 2);
            }
            if (c.short_name.empty()) {
                c.short_name = dlpguest::GameName(t.index.rows[r].game);
            }
            fresh.push_back(c);
        }
        // the Installed children of ours the index file has no row for: their hosts are grey ("index not written") and say which game it is
        for (const dlpguest::Guest& guest : dlpguest::LoadGuests()) {
            bool listed = false;
            for (const Child& known : fresh) {
                listed = listed || known.child == guest.child;
            }
            if (!listed && guest.state == dlpguest::GuestState::Installed && dlpguest::IsOwnedGuestTid(guest.prepared)) {
                Child c;
                c.child = guest.child;
                c.prepared = guest.prepared;
                c.short_name = dlpguest::GameName(guest.game);
                fresh.push_back(c);
            }
        }
        if (!children_.empty()) {
            gfx::ReleaseIcons();  // (a new copy: the textures keyed by the old one go)
        }
        children_.swap(fresh);
    }

    const Child* ChildOf(u32 child_low) const {
        const u64 child = (static_cast<u64>(azl::title_rules::kTitleHighDlpChild) << 32) | child_low;
        for (const Child& c : children_) {
            if (c.child == child) {
                return &c;
            }
        }
        return nullptr;
    }

    const Child* ChildByPrepared(u64 prepared) const {
        for (const Child& c : children_) {
            if (prepared != 0 && c.prepared == prepared) {
                return &c;
            }
        }
        return nullptr;
    }

    bool Leavable() const {
        return mode_ == Mode::Session;
    }

    std::string PhaseText() {
        if (mode_ == Mode::Handing) {
            return "Starting " + chosen_game_ + "...";
        }
        switch (session_.phase()) {
        case dlpjoin::Phase::Waiting: return "Waiting for " + chosen_name_ + " to start...";
        case dlpjoin::Phase::Starting: return "Starting...";
        case dlpjoin::Phase::Passphrase: return "Starting " + chosen_game_ + "...";
        default: return "Joining " + chosen_name_ + "...";
        }
    }

    void DrawChosen(const std::string& phase) {
        DrawCard(10, 32, 380, 168);
        gfx::FillRect(10, 32, 3, 168, col::kAccent);
        const Child* c = ChildOf(chosen_child_);
        if (c != nullptr && !c->icon.empty()) {
            gfx::DrawIcon(c->icon.data(), setup::kLargeIcon, 24, 46, 1.0f);
        }
        gfx::DrawWrapped(84, 46, 296, 0.5f, col::kText, chosen_game_, 2);
        gfx::DrawText(84, 92, 0.46f, col::kDim, gfx::Ellipsize("Hosted by " + chosen_name_, 0.46f, 296));
        gfx::DrawText(26, 128, 0.55f, col::kText, gfx::Ellipsize(phase, 0.55f, 350));
        // (DPAPP-2j-rev) the closed game of the guest is freed at the start of 3DS Link only: said before the jump
        gfx::DrawWrapped(26, 150, 354, 0.40f, col::kDim,
                         "The game starts when the host starts; nothing is downloaded. When you close it, open 3DS Link before another game: it frees "
                         "the memory the closed game keeps.",
                         3);
    }

    void DrawHost(const dlpjoin::Host& h) {
        const bool join = dlpjoin::ShowOf(h) == dlpjoin::Show::Join;
        const Child* c = ChildOf(h.g.child_tid_low);
        DrawCard(10, 32, 380, 168);
        gfx::FillRect(10, 32, 3, 168, join ? col::kOn : col::kDim);
        if (c != nullptr && !c->icon.empty()) {
            gfx::DrawIcon(c->icon.data(), setup::kLargeIcon, 24, 46, 1.0f, join ? 255 : 110);
        }
        const std::string game = c != nullptr ? (c->long_name.empty() ? c->short_name : c->long_name) : std::string("Another game");
        gfx::DrawWrapped(84, 46, 296, 0.5f, col::kText, game, 2);
        gfx::DrawText(84, 92, 0.46f, col::kText, gfx::Ellipsize("Hosted by " + HostName(h.g), 0.46f, 296));
        gfx::DrawText(26, 120, 0.46f, col::kText,
                      Fmt("Players %u of %u", static_cast<unsigned>(h.g.total_nodes), static_cast<unsigned>(h.g.max_nodes)));
        gfx::DrawText(382, 120, 0.42f, col::kDim, gfx::Ellipsize(gfx::Clean("Room " + RoomLabel()), 0.42f, 220), gfx::Align::Right);
        if (join) {
            gfx::DrawText(26, 142, 0.5f, col::kOn, "Ready: A joins");
            gfx::DrawWrapped(26, 158, 354, 0.40f, col::kDim,
                             "Its guest is on this console: nothing is downloaded. After the game, open 3DS Link before another game (it frees its memory).", 3); // (DPAPP-2j-rev)
        } else {
            gfx::DrawText(26, 142, 0.5f, col::kWarn, gfx::Ellipsize(std::string("Cannot join: ") + GreyValue(h), 0.5f, 350));
            gfx::DrawWrapped(26, 158, 354, 0.40f, col::kDim, GreyNote(h), 3);
        }
    }

    // The search as the rows of this frame found it (BuildRows): what the bars and the search card say, with no look of their own. Its title is the
    // room (the bar of the touch screen says how the search goes, the row what the list has: not the same words again - the review of 2026-11-18).
    void DrawSearch(const Row& row) const {
        DrawCard(10, 32, 380, 168);
        gfx::FillRect(10, 32, 3, 168, col::kAccent);
        gfx::DrawFitted(26, 48, 0.55f, 0.42f, 350, col::kAccent, gfx::Clean("Room " + RoomLabel()));
        const float cw = CaptionWidth();
        gfx::DrawText(26, 72, 0.46f, col::kDim, "Hosts");
        gfx::DrawText(26 + cw, 72, 0.46f, col::kText,
                      found_ == 0 ? std::string("none yet")
                                  : Fmt("%d found, %d you can join", found_, joinable_));
        gfx::DrawWrapped(26, 92, 350, 0.42f, col::kDim, row.note, 6);
    }

    dlpjoin::Session session_;
    dlp::WorkHolds holds_;
    Mode mode_ = Mode::List;
    bool begun_ = false;
    int waited_frames_ = 0; // (Mode::Waiting) frames since the work was last looked at
    Wait wait_ = Wait::Entry; // (Mode::Waiting) what for
    std::vector<dlpjoin::Host> hosts_;  // the rows of the hosts, in their order (the list of this frame)
    std::string search_ = "Looking for hosts...";  // the bar of the touch screen (BarTitle): how the search goes, from the rows of this frame
    int found_ = 0;   // the hosts of this frame's scan, listed or not
    int joinable_ = 0;  // ... that can be joined now (their row is not grey)
    std::vector<Child> children_;
    std::string chosen_name_;
    std::string chosen_game_;
    u32 chosen_child_ = 0;  // the low word of the chosen host's child (ChildOf): its icon is children_'s
    u32 refusals_at_ = 0;
    u32 rounds_at_ = 0;
    bool entered_ = false;
};

} // namespace

namespace pages {

ui::Page* NewDlpJoinPage() {
    return new DlpJoinPage;
}

} // namespace pages
