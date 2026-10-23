// Page: Test the connection. Joins the room from here the way the plugin does, shows who is in it, and leaves again.
// A successful test also moves the room to the top of the saved rooms. The join itself is a RoomProbe (probe.hpp), the same one the popup
// that tests a room just added runs.

#include "pages.hpp"
#include "probe.hpp"

using namespace ui;

namespace {

class TestPage : public Page {
public:
    TestPage() {
        probe_.Start(g.cfg.host, g.cfg.port, g.cfg.password);
    }

    void Update(const Input& in) override {
        probe_.Update();
        if (in.down & KEY_START) {
            g.quit = true;
            return;
        }
        if (probe_.phase() == pages::RoomProbe::Phase::Leaving) {
            if (probe_.leave_finished()) {
                if (restart_) {
                    restart_ = false;
                    probe_.Start(g.cfg.host, g.cfg.port, g.cfg.password);
                    noted_ = false;
                    list_ = ListView();
                } else {
                    PopPage();
                }
            }
            return;
        }
        if (probe_.phase() == pages::RoomProbe::Phase::Done && !noted_) {
            RoomWorks();
        }

        const bool finished = (probe_.phase() == pages::RoomProbe::Phase::Done || probe_.phase() == pages::RoomProbe::Phase::Failed);
        BuildRows();
        UpdateList(list_, rows_, in);

        if (in.down & KEY_B || Tapped(in, kLeftButton)) {
            BeginLeave(false);
        } else if (finished && !probe_.stopping() && (in.down & KEY_A || Tapped(in, kRightButton))) {
            BeginLeave(true);
        }
    }

    void DrawTop() override {
        DrawTitleBarStatus(gfx::kTopW, "Connection test");

        // What we are testing.
        const std::string target = g.cfg.host.empty() ? std::string("(not set)") : g.cfg.host + ":" + std::to_string(g.cfg.port);
        const float cw = CaptionWidth();
        const float col1 = 14.0f, col2 = 208.0f, right_edge = 392.0f;
        gfx::DrawText(col1, 38, 0.46f, col::kDim, "Room");
        gfx::DrawFitted(col1 + cw, 38, 0.46f, 0.36f, col2 - 8.0f - (col1 + cw), col::kText, gfx::Clean(target));
        gfx::DrawText(col2, 38, 0.46f, col::kDim, "WiFi");
        DrawNetworkLine(col2 + cw, 38, right_edge - (col2 + cw));
        gfx::DrawText(col1, 54, 0.46f, col::kDim, "Name");
        gfx::DrawFitted(col1 + cw, 54, 0.46f, 0.36f, col2 - 8.0f - (col1 + cw), col::kText, gfx::Clean(probe_.nickname()));
        gfx::DrawText(col2, 54, 0.46f, col::kDim, "MAC");
        gfx::DrawFitted(col2 + cw, 54, 0.46f, 0.34f, right_edge - (col2 + cw), col::kText, setup::MacToString(probe_.console_mac()));

        // Steps.
        DrawCard(10, 66, 380, 62);
        pages::DrawProbeStep(probe_, 24, 80, 0, "Reach the room (UDP " + std::to_string(g.cfg.port) + ")");
        pages::DrawProbeStep(probe_, 24, 96, 1, "Join with the console's name and MAC");
        pages::DrawProbeStep(probe_, 24, 112, 2, "Receive the room's member list");

        // Result.
        const pages::RoomProbe::Phase phase = probe_.phase();
        if (phase == pages::RoomProbe::Phase::Failed) {
            DrawCard(10, 136, 380, 98);
            gfx::FillRect(10, 136, 3, 98, col::kOff);
            gfx::DrawText(24, 152, 0.6f, col::kOff, gfx::Ellipsize(probe_.error(), 0.6f, 350));
            gfx::DrawWrapped(24, 166, 358, 0.44f, col::kText, probe_.hint(), 5);
        } else if (phase == pages::RoomProbe::Phase::Running || (phase == pages::RoomProbe::Phase::Leaving && !probe_.joined_info())) {
            DrawCard(10, 136, 380, 98);
            const int dots = static_cast<int>(probe_.frame() / 20) % 4;
            std::string text = phase == pages::RoomProbe::Phase::Leaving ? "Leaving" : "Connecting";
            text.append(static_cast<std::size_t>(dots), '.');
            gfx::DrawText(24, 152, 0.58f, col::kText, text);
            gfx::DrawWrapped(24, 168, 358, 0.43f, col::kDim, "This only joins the room and leaves again; it does not touch any title.", 3);
        } else {
            DrawRoomInfo();
        }
        DrawFrameBorder(gfx::kTopW);
    }

    void DrawBottom() override {
        DrawRows(list_, rows_);
        std::string right;
        if (probe_.joined_info()) {
            right = Fmt("%u / %u", static_cast<unsigned>(probe_.info().members.size()), static_cast<unsigned>(probe_.info().member_slots));
        }
        DrawListChrome("Players in the room", right);
        const Input& in = g.input.in();
        const pages::RoomProbe::Phase phase = probe_.phase();
        const bool finished = (phase == pages::RoomProbe::Phase::Done || phase == pages::RoomProbe::Phase::Failed);
        DrawButton(kLeftButton, "B", phase == pages::RoomProbe::Phase::Leaving ? "Leaving..." : "Close",
                   phase != pages::RoomProbe::Phase::Leaving, PressedNow(in, kLeftButton));
        DrawButton(kRightButton, "A", "Test again", finished && !probe_.stopping(), PressedNow(in, kRightButton) && finished);
    }

private:
    void DrawRoomInfo() {
        const azl::room::RoomInfo& info = probe_.info();
        DrawCard(10, 136, 380, 98);
        gfx::FillRect(10, 136, 3, 98, col::kOn);
        const std::string name = gfx::Clean(info.name.empty() ? std::string("(unnamed room)") : info.name);
        gfx::DrawText(24, 150, 0.6f, col::kText, gfx::Ellipsize(name, 0.6f, 356));
        float y = 161.0f;
        if (!info.description.empty()) {
            y += gfx::DrawWrapped(24, y, 358, 0.42f, col::kDim, gfx::Clean(info.description), 2);
        }
        const float lh = gfx::LineHeight(0.45f);
        // A dedicated azahar-room server has no host player.
        const std::string host = info.host_username.empty() ? std::string("(dedicated server)") : gfx::Clean(info.host_username);
        gfx::DrawText(24, y + lh * 0.5f, 0.45f, col::kText,
                      gfx::Ellipsize(Fmt("Players %u / %u    Host: %s", static_cast<unsigned>(info.members.size()),
                                         static_cast<unsigned>(info.member_slots), host.c_str()),
                                     0.45f, 356));
        gfx::DrawText(24, y + lh * 1.5f, 0.45f, col::kText,
                      gfx::Ellipsize("Preferred game: " + gfx::Clean(info.preferred_game.empty() ? std::string("any") : info.preferred_game),
                                     0.45f, 356));
        if (!probe_.closed_note().empty()) {
            gfx::DrawText(24, y + lh * 2.5f, 0.45f, col::kWarn,
                          gfx::Ellipsize("The room closed the connection: " + probe_.closed_note(), 0.45f, 356));
        } else {
            gfx::DrawFitted(24, y + lh * 2.5f, 0.45f, 0.36f, 356, col::kDim, "My MAC in the room: " + setup::MacToString(probe_.room_mac()));
        }
    }

    // The test passed: the room that was tested goes to the top of the saved rooms (and into the list when it was not there).
    void RoomWorks() {
        noted_ = true;
        setup::Server tested;
        tested.host = g.cfg.host;
        tested.port = g.cfg.port;
        tested.password = g.cfg.password; // (the test used it: the room in the list keeps it)
        if (g.servers.Use(tested)) {
            g.SaveServers();
        }
    }

    void BeginLeave(bool restart) {
        if (probe_.phase() == pages::RoomProbe::Phase::Leaving) {
            return;
        }
        restart_ = restart;
        probe_.BeginLeave();
    }

    void BuildRows() {
        rows_.clear();
        const pages::RoomProbe::Phase phase = probe_.phase();
        const azl::room::RoomInfo& info = probe_.info();
        if (!probe_.joined_info() || info.members.empty()) {
            Row waiting;
            waiting.kind = Kind::Info;
            waiting.label = phase == pages::RoomProbe::Phase::Failed ? "No member list" : "Waiting for the room...";
            waiting.note_title = "Players";
            waiting.note = "The members of the room appear here once the room has answered.";
            rows_.push_back(waiting);
            return;
        }
        for (const azl::room::MemberInfo& m : info.members) {
            Row row;
            row.kind = Kind::Info;
            const bool me = (m.mac == probe_.room_mac());
            row.label = gfx::Clean(m.nickname) + (me ? "  (you)" : "");
            row.value = gfx::Clean(m.game_name.empty() ? std::string("-") : m.game_name);
            row.note_title = gfx::Clean(m.nickname);
            rows_.push_back(row);
        }
    }

    pages::RoomProbe probe_;
    bool restart_ = false;
    bool noted_ = false;
    ListView list_;
    std::vector<Row> rows_;
};

} // namespace

namespace pages {

ui::Page* NewTestPage() {
    return new TestPage;
}

} // namespace pages
