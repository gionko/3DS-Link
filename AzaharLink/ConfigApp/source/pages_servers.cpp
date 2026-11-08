// Page: Azahar Emulator Room. The rooms the user has saved (at most 10, the most recently used first). A room is ONE thing with four parts: its name, its
// address, its port and its password; each room keeps its own password. A uses a room (it is written to AzaharLink.ini at once, with its
// password), Y changes the room under the cursor (the same four fields as when it was added) or, on the last row, adds one, X removes one.
// Under the saved rooms, "Public lobbies" opens the public rooms (pages_lobbies.cpp): its dot says that the room in use is one of them.
// The list itself is servers.txt in the app's data folder. A room that is new or whose address, port or password changed is tested at once (a
// short popup with the steps of the connection test): when it answers it becomes the room in use, when it does not the user is asked whether
// to use it anyway.
#include <cstdlib>

#include "pages.hpp"

using namespace ui;

namespace {

// ---------------------------------------------------------------------------------------------
// The four fields of a room
// ---------------------------------------------------------------------------------------------

class RoomFormPage : public ListPage {
public:
    using SavedFn = std::function<void(const setup::Server& room)>;

    // `room` is what the form starts with (an empty room for a new one); `form` changes the words and, for a public room, keeps the address.
    RoomFormPage(const setup::Server& room, pages::RoomForm form, SavedFn saved) : room_(room), form_(form), saved_(std::move(saved)) {
        port_text_ = std::to_string(room_.port);
    }

    void DrawTop() override {
        DrawTitleBarStatus(gfx::kTopW, Title());
        DrawCard(10, 32, 380, 70);
        gfx::FillRect(10, 32, 3, 70, col::kAccent);
        const char* heading = form_ == pages::RoomForm::Add          ? "Add a room"
                              : form_ == pages::RoomForm::Change     ? "Change the room"
                                                                     : "A public room among yours";
        const char* text = form_ == pages::RoomForm::Favourite
                               ? "Its address, port and password come with the list of public rooms: only its name can be changed here. Press Y to "
                                 "save."
                               : "A room has a name, an address, a port and a password, all in one place. Fill them in (A on a row), then press Y to "
                                 "save: the room is tested at once.";
        gfx::DrawText(26, 46, 0.55f, col::kAccent, heading);
        gfx::DrawWrapped(26, 60, 354, 0.43f, col::kText, text, 3);
        if (const Row* row = Selected()) {
            DrawNote(10, 108, 380, 90, *row);
        }
        DrawStatusStrip(10, 206, 380, 28);
        DrawFrameBorder(gfx::kTopW);
    }

protected:
    const char* Title() const override {
        return "Azahar Emulator Room";
    }
    const char* BarTitle() const override {
        return "The room";
    }
    const char* ActionLabel() const override {
        const Row* row = Selected();
        return row && row->id == kTest ? "Test" : "Edit";
    }
    const char* BackLabel() const override {
        return "Cancel";
    }
    int ExtraKeys(FooterKey* out) const override {
        out[0] = FooterKey{"Y", "Save", KEY_Y};
        return 1;
    }
    void OnExtra(int, const Row*) override {
        Save();
    }

    void BuildRows(std::vector<Row>& rows) override {
        rows.clear();
        Row name;
        name.id = kName;
        name.kind = Kind::Value;
        name.label = "Name";
        name.value = room_.name.empty() ? "none" : gfx::Clean(room_.name);
        name.note_title = "Name of the room";
        name.note = Fmt("Up to %u letters, digits, space . _ - (it can stay empty: the list then shows the address).",
                        static_cast<unsigned>(setup::kMaxServerNameChars));
        rows.push_back(name);

        Row host;
        host.id = kHost;
        host.kind = Kind::Value;
        host.label = "Address";
        host.value = room_.host.empty() ? "not set" : gfx::Clean(room_.host);
        host.value_color = room_.host.empty() ? col::kWarn : (locked() ? col::kDim : 0);
        host.note_title = "Address of the room";
        host.note = locked() ? "The address of a public room comes with the list: it cannot be changed here."
                             : "The IP address (or the name) of the Azahar room's server, without the port.";
        rows.push_back(host);

        Row port;
        port.id = kPort;
        port.kind = Kind::Value;
        port.label = "Port";
        port.value = port_text_;
        port.value_color = locked() ? col::kDim : 0;
        port.note_title = "Port";
        port.note = Fmt("The UDP port of the room, 1 to 65535. The usual one is %u.", static_cast<unsigned>(azl::room::kDefaultPort));
        rows.push_back(port);

        Row pass;
        pass.id = kPassword;
        pass.kind = Kind::Value;
        pass.label = "Password";
        pass.value = room_.password.empty() ? "none" : "set";
        pass.value_color = locked() ? col::kDim : 0;
        pass.note_title = "Password of the room";
        pass.note = locked() ? "The password of a public room is the one it was kept with (Public lobbies asks it when the room wants one): it "
                               "cannot be changed here."
                             : "Only needed when this room is password protected. Each room keeps its own: leave it empty otherwise.";
        rows.push_back(pass);

        Row test;
        test.id = kTest;
        test.kind = Kind::Link;
        test.label = "Test room";
        test.gap = 6.0f;
        test.note_title = "Test the room";
        test.note = "Joins the room with the address, port and password above, to see that it answers, and shows what happened (or why it "
                    "did not). Nothing is saved by the test.";
        rows.push_back(test);
    }

    void Activate(const Row& row, int step) override {
        if (step != 0) {
            return;
        }
        std::string input;
        if (locked() && (row.id == kHost || row.id == kPort || row.id == kPassword)) {
            g.ShowToast("A public room: only its name can be changed.", col::kWarn, 4000);
            return;
        }
        switch (row.id) {
        case kName:
            input = room_.name;
            if (g.EditText(Fmt("Name of the room (up to %u letters, empty = none)", static_cast<unsigned>(setup::kMaxServerNameChars)).c_str(), input,
                           static_cast<int>(setup::kMaxServerNameChars), true)) {
                input = TrimSpaces(input);
                if (setup::ValidServerName(input)) {
                    room_.name = input;
                } else {
                    g.ShowToast(Fmt("Invalid name: up to %u of A-Z a-z 0-9 space . _ -", static_cast<unsigned>(setup::kMaxServerNameChars)), col::kOff, 5000);
                }
            }
            break;
        case kHost:
            input = room_.host;
            if (g.EditText("Address of the room (IP or name, without the port)", input, 80, false)) {
                setup::Server probe;
                const std::string text = TrimSpaces(input);
                if (text.find(':') == std::string::npos && setup::ParseServer(text, probe) == setup::ServerParse::Ok) {
                    room_.host = probe.host;
                } else {
                    g.ShowToast("Invalid address (the port goes in its own row).", col::kOff, 5000);
                }
            }
            break;
        case kPort:
            input = port_text_;
            if (g.EditText("Port of the room (1 to 65535)", input, 5, false, true)) {
                const long port = std::strtol(TrimSpaces(input).c_str(), nullptr, 10);
                if (port >= 1 && port <= 65535) {
                    room_.port = static_cast<u16>(port);
                    port_text_ = std::to_string(port);
                } else {
                    g.ShowToast("Invalid port (1-65535).", col::kOff, 4000);
                }
            }
            break;
        case kPassword:
            input = room_.password;
            if (g.EditText("Password of the room (empty = none)", input, 64, true)) {
                room_.password = input;
            }
            break;
        case kTest:
            if (room_.host.empty()) {
                g.ShowToast("Fill in the address first (A on the Address row).", col::kWarn, 4000);
            } else {
                PushPage(pages::NewProbePage(room_, nullptr)); // (the page keeps the result on the screen until it is closed)
            }
            break;
        default:
            break;
        }
    }

private:
    enum Id { kName = 1, kHost, kPort, kPassword, kTest };

    void Save() {
        setup::Server checked;
        if (room_.host.empty() || setup::ParseServer(room_.host + ":" + port_text_, checked) != setup::ServerParse::Ok) {
            g.ShowToast("Fill in the address first (A on the Address row).", col::kWarn, 4000);
            return;
        }
        PopPage();
        if (saved_) {
            saved_(room_);
        }
    }

    bool locked() const {
        return form_ == pages::RoomForm::Favourite; // (a public room's address, port and password come with the list)
    }

    setup::Server room_;
    std::string port_text_;
    pages::RoomForm form_;
    SavedFn saved_;
};

// ---------------------------------------------------------------------------------------------
// The list of rooms
// ---------------------------------------------------------------------------------------------

class ServersPage : public ListPage {
public:
    ServersPage() {
        public_in_use_ = pages::RefreshPublicRoomName();
    }
    void OnResume() override {
        public_in_use_ = pages::RefreshPublicRoomName(); // (a public room may have been chosen meanwhile)
        if (from_lobbies_) {
            // (back from Public lobbies the cursor is on its row again: a favourite added there moved the row one down)
            from_lobbies_ = false;
            list_.sel = static_cast<int>(g.servers.size());
        }
    }

protected:
    const char* Title() const override {
        return "Azahar Emulator Room";
    }
    const char* BarTitle() const override {
        return "Server list";
    }
    std::string BarRight() const override {
        return Fmt("%u / %u", static_cast<unsigned>(g.servers.size()), static_cast<unsigned>(setup::ServerList::kMax));
    }
    const char* ActionLabel() const override {
        return "Use";
    }
    bool StartButton() const override {
        return false;
    }
    int ExtraKeys(FooterKey* out) const override {
        const Row* row = Selected();
        const bool on_room = row && row->id >= kServerBase;
        out[0] = FooterKey{"X", "Remove", KEY_X};
        out[1] = FooterKey{"Y", on_room ? "Edit" : "Add", KEY_Y};
        return 2;
    }
    void OnExtra(int index, const Row* selected) override {
        if (index == 0) {
            if (selected && selected->id >= kServerBase) {
                AskRemove(static_cast<std::size_t>(selected->id - kServerBase));
            }
        } else if (selected && selected->id >= kServerBase) {
            Edit(static_cast<std::size_t>(selected->id - kServerBase));
        } else {
            AddServer();
        }
    }

    void BuildRows(std::vector<Row>& rows) override {
        rows.clear();
        setup::Server active;
        active.host = g.cfg.host;
        active.port = g.cfg.port;
        const std::vector<setup::Server>& servers = g.servers.entries();
        for (std::size_t i = 0; i < servers.size(); ++i) {
            const bool in_use = servers[i].Same(active);
            Row row;
            row.id = kServerBase + static_cast<int>(i);
            row.kind = Kind::Action;
            // The name, with the address at its side; a room without a name shows its address alone.
            row.label = gfx::Clean(servers[i].Label());
            row.value = servers[i].name.empty() ? std::string() : gfx::Clean(servers[i].Text());
            row.mark = in_use ? Mark::Dot : Mark::Slot;
            row.note_title = row.label;
            row.note = std::string(in_use ? "The room 3DS Link joins when a title starts (the dot).\n" : "") +
                       "Address: " + gfx::Clean(servers[i].Text()) + "\nPassword: " + (servers[i].password.empty() ? "none" : "set") + "\n" +
                       "A: use this room (its password too). Y: change it. X: remove it.";
            rows.push_back(row);
        }
        Row lobbies;
        lobbies.id = kPublic;
        lobbies.kind = Kind::Link;
        lobbies.label = "Public lobbies";
        lobbies.mark = public_in_use_ ? Mark::Dot : Mark::Slot;
        lobbies.gap = servers.empty() ? 0.0f : 6.0f;
        // (the warning is here, before the page opens: the page itself gives its whole top screen to the rooms)
        lobbies.note_title = "Public rooms: use them at your own risk";
        lobbies.note_color = col::kOff;
        lobbies.note = std::string(public_in_use_ ? "The room in use is a public room (the dot): " + gfx::Clean(RoomLabel()) + ".\n" : "") +
                       "Other people put these Azahar rooms online: they are not part of 3DS Link. Nobody knows who is inside, whether they "
                       "cheat, or whether somebody uses software against your console. Your nickname and your console's MAC can be seen by "
                       "the room.\n\nA room with a low ping plays better: the game runs smoother, with less lag. In general the best range is "
                       "15 to 38 ms.";
        rows.push_back(lobbies);

        Row add;
        add.id = kAdd;
        add.kind = Kind::Action;
        add.label = "Add a room...";
        add.mark = Mark::Slot;
        add.gap = 0.0f;
        add.note_title = "Add a room";
        add.note = std::string(servers.empty() ? "No room saved yet.\n" : "") +
                   "A page with the room's name, address, port and password. The new room is tested at once and, when it answers, used from "
                   "now on. The list keeps 10 rooms.";
        rows.push_back(add);
    }

    void Activate(const Row& row, int step) override {
        if (step != 0) {
            return;
        }
        if (row.id == kAdd) {
            AddServer();
        } else if (row.id == kPublic) {
            from_lobbies_ = true;
            PushPage(pages::NewLobbiesPage());
        } else if (row.id >= kServerBase) {
            Use(static_cast<std::size_t>(row.id - kServerBase));
        }
    }

private:
    enum Id { kAdd = 1, kPublic = 2, kServerBase = 100 };
    bool public_in_use_ = false;
    bool from_lobbies_ = false; // Public lobbies was opened: the cursor goes back to its row when it closes

    // The ini keeps the room 3DS Link joins (with its password); the list only remembers rooms.
    void Select(const setup::Server server) { // (by value: the list is rearranged below)
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
        list_.sel = 0; // the room is on top now: the cursor goes with it
        list_.follow = true;
        public_in_use_ = pages::RefreshPublicRoomName();
    }

    void Use(std::size_t index) {
        if (index < g.servers.size()) {
            Select(g.servers.entries()[index]);
        }
    }

    void AskRemove(std::size_t index) {
        if (index >= g.servers.size()) {
            return;
        }
        const std::string label = gfx::Clean(g.servers.entries()[index].Label());
        PushPage(pages::NewConfirmPage("Remove the room", "Remove \"" + label + "\" from the list? Its address and its password are forgotten.",
                                       "Yes, remove it", "No", [this, index](bool yes) {
                                           if (yes && index < g.servers.size()) {
                                               const std::string text = g.servers.entries()[index].Label();
                                               g.servers.Remove(index);
                                               g.SaveServers();
                                               g.ShowToast("Removed " + gfx::Clean(text), col::kText);
                                           }
                                       }));
    }

    void Edit(std::size_t index) {
        if (index >= g.servers.size()) {
            return;
        }
        const setup::Server room = g.servers.entries()[index];
        // (a public room kept with X on Public lobbies: only its name can change)
        const pages::RoomForm form = pages::InPublicList(room.host, room.port) ? pages::RoomForm::Favourite : pages::RoomForm::Change;
        PushPage(new RoomFormPage(room, form, [this, index](const setup::Server& changed) { Saved(static_cast<int>(index), changed); }));
    }

    void AddServer() {
        if (g.servers.full()) {
            g.ShowToast("10 rooms: remove one first (X)", col::kWarn, 4000);
            return;
        }
        PushPage(new RoomFormPage(setup::Server(), pages::RoomForm::Add, [this](const setup::Server& room) { Saved(-1, room); }));
    }

    // The form was saved: the room is in the list (new, or in the place of the one that was changed) and, when it is new or its address, port
    // or password changed, tested. The room that was in use and was changed stays the room in use (with the new details).
    void Saved(int index, const setup::Server& room) {
        setup::Server active;
        active.host = g.cfg.host;
        active.port = g.cfg.port;
        bool was_active = false;
        bool changed = true;
        if (index >= 0) {
            const setup::Server old = g.servers.entries()[static_cast<std::size_t>(index)];
            was_active = old.Same(active);
            changed = !old.Same(room) || old.password != room.password;
            if (g.servers.Replace(static_cast<std::size_t>(index), room) == setup::ServerList::ReplaceResult::Duplicate) {
                g.ShowToast("Another room has that address and port.", col::kOff, 5000);
                return;
            }
        } else if (g.servers.Add(room) == setup::ServerList::AddResult::Full) {
            g.ShowToast("10 rooms: remove one first (X)", col::kWarn, 4000);
            return;
        }
        g.SaveServers();
        if (was_active) {
            g.cfg.host = room.host;
            g.cfg.port = room.port;
            g.cfg.password = room.password;
            g.Save();
        }
        list_.sel = 0;
        list_.follow = true;
        if (!changed) {
            g.ShowToast("Saved.", col::kOn);
            return;
        }
        PushPage(pages::NewProbePage(room, [this, room, was_active](pages::ProbeOutcome outcome) {
            if (outcome == pages::ProbeOutcome::Reached) {
                if (was_active) {
                    g.ShowToast("Saved: the room answers.", col::kOn, 4000);
                } else {
                    Select(room);
                }
            } else if (outcome == pages::ProbeOutcome::Failed || outcome == pages::ProbeOutcome::WrongPassword) {
                if (was_active) {
                    g.ShowToast("Saved, but the room could not be reached.", col::kWarn, 5000);
                    return;
                }
                PushPage(pages::NewConfirmPage("Connection test failed",
                                               "Could not reach the room. Use it as the active room anyway?", "Yes, use it", "No",
                                               [this, room](bool yes) {
                                                   if (yes) {
                                                       Select(room);
                                                   } else {
                                                       g.ShowToast("Saved, but not the active room.", col::kText, 4000);
                                                   }
                                               }));
            } else {
                g.ShowToast("Test cancelled: the room is saved.", col::kText, 4000);
            }
        }));
    }
};

} // namespace

namespace pages {

ui::Page* NewServersPage() {
    return new ServersPage;
}

ui::Page* NewRoomFormPage(const setup::Server& room, RoomForm form, std::function<void(const setup::Server& room)> saved) {
    return new RoomFormPage(room, form, std::move(saved));
}

} // namespace pages
