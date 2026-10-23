#include "probe.hpp"

#include <cmath>
#include <cstdio>

#include "pages.hpp"

using azl::RoomClient;
using namespace ui;

namespace pages {

namespace {

// ---------------------------------------------------------------------------------------------
// Async leave: RoomClient::Stop() joins the network thread, which can take a moment (or longer
// while a host name is being resolved). It runs on a helper thread so the screen keeps rendering.
// ---------------------------------------------------------------------------------------------

constexpr u32 kStopWaitMs = 5000;

// Stops a client and frees it. A client whose network thread did not leave in time is NOT freed: that thread still uses it (it can be stuck
// resolving a name), and a few KB that stay are better than a write into memory that is gone.
void StopAndFree(RoomClient* client) {
    if (client->StopWithin(kStopWaitMs)) {
        delete client;
    }
}

void StopThread(void* arg) {
    StopJob* job = static_cast<StopJob*>(arg);
    StopAndFree(job->client);
    job->client = nullptr;
    job->done.store(true); // last access to `job`
}

// Takes ownership of `client`.
StopJob* BeginAsyncStop(RoomClient* client) {
    StopJob* job = new StopJob;
    job->client = client;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    Thread thread = threadCreate(StopThread, job, 0x8000, priority, -2, true);
    if (!thread) {
        StopThread(job); // could not start a thread: do it right here
    }
    return job;
}

// Used at exit only.
void WaitStopJob(StopJob* job) {
    const u64 start = NowMs();
    while (!job->done.load() && NowMs() - start < 6000) {
        svcSleepThread(5 * 1000 * 1000);
    }
}

std::string HintFor(RoomClient::Error e) {
    switch (e) {
    case RoomClient::Error::CouldNotConnect:
        return "No answer from the room. Check the address and port, that the Azahar room is running, the "
               "port forwarding / firewall on that PC, and that this console is on the same network or can "
               "reach it.";
    case RoomClient::Error::NameCollision:
        return "Another player already uses this nickname. Choose a different nickname, or leave it empty to "
               "use the console's profile name.";
    case RoomClient::Error::MacCollision:
        return "A console with this MAC address is already in the room. Wait a moment (a game may still be "
               "connected) and test again.";
    case RoomClient::Error::ConsoleIdCollision:
        return "This console is already in the room (a game with 3DS Link may still be connected). Wait a "
               "moment and test again.";
    case RoomClient::Error::WrongVersion:
        return "The room speaks a different protocol version than 3DS Link supports (room protocol 4, as "
               "used by Azahar and Citra).";
    case RoomClient::Error::WrongPassword:
        return "The room is password protected and the password is wrong or missing. Change it in the room's details (Azahar Emulator Room, Y).";
    case RoomClient::Error::RoomIsFull:
        return "The room has no free slot. Ask the host to raise the member limit or wait for someone to leave.";
    case RoomClient::Error::HostBanned:
        return "The room's host banned this console or nickname.";
    case RoomClient::Error::HostKicked:
        return "The room's host removed this console from the room.";
    case RoomClient::Error::LostConnection:
        return "The connection to the room was lost or the room closed. Check the WiFi signal and test again.";
    case RoomClient::Error::RoomClosed:
        return "The room's host closed the room. Test again when it is open.";
    case RoomClient::Error::NetworkUnavailable:
        return "The console could not open a network socket. Check that the WiFi is connected, then test again.";
    default:
        return "The room did not accept the connection. Test again.";
    }
}

} // namespace

RoomProbe::~RoomProbe() {
    // Only reached at exit or after the stop finished: blocking here is fine.
    if (stop_) {
        WaitStopJob(stop_);
        if (stop_->done.load()) {
            delete stop_;
        }
        stop_ = nullptr;
    }
    if (client_) {
        StopAndFree(client_);
        client_ = nullptr;
    }
}

void RoomProbe::Start(const std::string& host, u16 port, const std::string& password) {
    net_ = setup::ReadNetwork(g.ac_ready, g.soc_ok());
    console_mac_ = setup::ReadConsoleMac();
    nickname_ = g.EffectiveNickname();
    error_.clear();
    hint_.clear();
    closed_note_.clear();
    info_ = azl::room::RoomInfo();
    room_mac_ = azl::Mac{};
    joined_info_ = false;
    joined_ms_ = 0;
    failed_step_ = 0;
    info_poll_ = 0;
    phase_ = Phase::Running;
    start_ms_ = NowMs();

    if (host.empty()) {
        Fail("Azahar Emulator Room not set", "Go back and enter the address of the Azahar room first.", 0);
        return;
    }
    if (!g.soc_ok()) {
        Fail("Network layer not available",
             Fmt("The console's socket service could not be started (error 0x%08lX). Close the app and try again.",
                 static_cast<unsigned long>(g.soc_rc)),
             0);
        return;
    }
    if (net_.known && !net_.wifi) {
        Fail("WiFi is not connected",
             "Connect the console to your WiFi network (System Settings > Internet Settings) and test again.", 0);
        return;
    }

    RoomClient* client = new RoomClient;
    RoomClient::Config rc;
    rc.host = host;
    rc.port = port;
    rc.nickname = nickname_;
    rc.password = password;
    rc.preferred_mac = console_mac_;
    char id[40];
    std::snprintf(id, sizeof(id), "3ds-%02x%02x%02x%02x%02x%02x-setup", console_mac_[0], console_mac_[1],
                  console_mac_[2], console_mac_[3], console_mac_[4], console_mac_[5]);
    rc.console_id_hash = id;
    rc.game_name = "3DS Link";
    rc.game_id = 0;
    if (!client->Start(rc)) {
        delete client;
        Fail("Could not start the network thread", "Close other applications and try again.", 0);
        return;
    }
    client_ = client;
}

void RoomProbe::Fail(const std::string& error, const std::string& hint, int step) {
    error_ = error;
    hint_ = hint;
    failed_step_ = step;
    phase_ = Phase::Failed;
    if (client_) {
        stop_ = BeginAsyncStop(client_); // the thread may still be waiting for the room
        client_ = nullptr;
    }
}

int RoomProbe::step() const {
    if (shown_phase() == Phase::Done) {
        return 3;
    }
    if (!client_) {
        return failed_step_;
    }
    switch (client_->state()) {
    case RoomClient::State::Connecting:
        return 0;
    case RoomClient::State::Joining:
        return 1;
    case RoomClient::State::Joined:
        return joined_info_ ? 3 : 2;
    default:
        return failed_step_;
    }
}

void RoomProbe::Update() {
    ++frame_;
    if (stop_ && stop_->done.load()) {
        delete stop_;
        stop_ = nullptr;
    }
    if (phase_ == Phase::Leaving) {
        return;
    }
    UpdateSession();
}

void RoomProbe::UpdateSession() {
    if (!client_) {
        return;
    }
    const u64 now = NowMs();
    const RoomClient::State state = client_->state();
    const RoomClient::Error err = client_->error();

    if (state == RoomClient::State::Joined) {
        if (joined_ms_ == 0) {
            joined_ms_ = now;
        }
        if (--info_poll_ <= 0) {
            info_poll_ = 20;
            info_ = client_->room_info();
            room_mac_ = client_->mac();
            joined_info_ = !info_.name.empty() || !info_.members.empty();
        }
        if (phase_ == Phase::Running && joined_info_ && now - joined_ms_ > 800) {
            phase_ = Phase::Done;
        } else if (phase_ == Phase::Running && now - joined_ms_ > 5000) {
            joined_info_ = true; // joined, but the room never sent its description
            phase_ = Phase::Done;
        }
        return;
    }
    if (state == RoomClient::State::Idle) {
        if (err != RoomClient::Error::None) {
            const int step = failed_step_ > 0 ? failed_step_ : (joined_ms_ ? 2 : 1);
            if (phase_ == Phase::Done) {
                // The room closed on us after a successful test: keep what we learned.
                closed_note_ = RoomClient::ErrorName(err);
                return;
            }
            Fail(RoomClient::ErrorName(err), HintFor(err),
                 (err == RoomClient::Error::CouldNotConnect || err == RoomClient::Error::NetworkUnavailable) ? 0 : step);
        } else if (now - start_ms_ > 1000) {
            Fail("Connection ended", "The connection ended before the room answered. Test again.", 0);
        }
        return;
    }
    if (phase_ == Phase::Running && now - start_ms_ > 12000) {
        Fail("No answer from the room (timeout)", HintFor(RoomClient::Error::CouldNotConnect),
             state == RoomClient::State::Connecting ? 0 : 1);
    }
}

void RoomProbe::BeginLeave() {
    if (phase_ == Phase::Leaving) {
        return;
    }
    end_phase_ = phase_;
    phase_ = Phase::Leaving;
    if (client_) {
        stop_ = BeginAsyncStop(client_);
        client_ = nullptr;
    }
}

void DrawProbeStep(const RoomProbe& probe, float x, float yc, int index, const std::string& text) {
    // reached: 0 = nothing done yet, 1 = reached the server, 2 = joined, 3 = got the member list
    const int reached = probe.step();
    const RoomProbe::Phase phase = probe.shown_phase();
    const bool done = reached > index;
    const bool active = reached == index && phase == RoomProbe::Phase::Running;
    const bool failed = reached == index && phase == RoomProbe::Phase::Failed;
    if (done) {
        gfx::FillCircle(x, yc, 6.0f, col::kOn);
        gfx::Line(x - 3.0f, yc, x - 1.0f, yc + 3.0f, 2.0f, col::kBg);
        gfx::Line(x - 1.0f, yc + 3.0f, x + 3.5f, yc - 3.0f, 2.0f, col::kBg);
    } else if (failed) {
        gfx::FillCircle(x, yc, 6.0f, col::kOff);
        gfx::Line(x - 3.0f, yc - 3.0f, x + 3.0f, yc + 3.0f, 2.0f, col::kBg);
        gfx::Line(x - 3.0f, yc + 3.0f, x + 3.0f, yc - 3.0f, 2.0f, col::kBg);
    } else if (active) {
        const float t = static_cast<float>(probe.frame() % 40) / 40.0f;
        for (int i = 0; i < 3; ++i) {
            const float p = std::fabs(t * 3.0f - static_cast<float>(i));
            const u8 alpha = static_cast<u8>(Clamp(255.0f - p * 120.0f, 70.0f, 255.0f));
            gfx::FillCircle(x - 6.0f + 6.0f * static_cast<float>(i), yc, 2.0f, gfx::WithAlpha(col::kAccent, alpha));
        }
    } else {
        gfx::FillCircle(x, yc, 4.0f, col::kTrack);
    }
    gfx::DrawText(x + 18.0f, yc, 0.48f, (done || active || failed) ? col::kText : col::kDim, text);
}

namespace {

// ---------------------------------------------------------------------------------------------
// The popup that tests a room (one just added, or the one in a form): when it is over it stays on the screen with what was found, or why it
// failed, until the user closes it (A or B)
// ---------------------------------------------------------------------------------------------

class ProbePage : public Page {
public:
    ProbePage(const setup::Server& server, ProbeFn done) : server_(server), done_(std::move(done)) {
        probe_.Start(server_.host, server_.port, server_.password);
    }

    void Update(const Input& in) override {
        probe_.Update();
        if (in.down & KEY_START) {
            g.quit = true;
            return;
        }
        const bool close_key = (in.down & (KEY_A | KEY_B)) || Tapped(in, Button());
        if (!ended_) {
            if (close_key) {
                Finish(ProbeOutcome::Cancelled);
            } else if (probe_.phase() == RoomProbe::Phase::Done) {
                Finish(ProbeOutcome::Reached);
            } else if (probe_.phase() == RoomProbe::Phase::Failed) {
                Finish(ProbeOutcome::Failed);
            }
        }
        // A cancelled test closes by itself when the room is left; one that ended on its own waits to be read.
        if (ended_ && probe_.leave_finished() && (outcome_ == ProbeOutcome::Cancelled || (close_key && was_ended_))) {
            PopPage();
            if (done_) {
                done_(outcome_);
            }
        }
        was_ended_ = ended_; // (the key that ended the test is not the key that closes the result)
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
        constexpr float kCardY = 18.0f, kCardH = 214.0f;
        DrawCard(8, kCardY, 304, kCardH); // (the card holds its button)
        gfx::FillRect(8, kCardY, 3, kCardH, col::kAccent);
        gfx::DrawText(22, 34, 0.55f, col::kAccent, "Testing the room");
        gfx::DrawFitted(22, 52, 0.46f, 0.34f, 280, col::kText, gfx::Clean(server_.Label() + (server_.name.empty() ? "" : "  " + server_.Text())));
        DrawProbeStep(probe_, 30, 74, 0, "Reach the room (UDP " + std::to_string(server_.port) + ")");
        DrawProbeStep(probe_, 30, 92, 1, "Join with the console's name and MAC");
        DrawProbeStep(probe_, 30, 110, 2, "Receive the room's member list");
        if (probe_.phase() == RoomProbe::Phase::Failed || (ended_ && outcome_ == ProbeOutcome::Failed)) {
            gfx::DrawFitted(22, 132, 0.48f, 0.36f, 280, col::kOff, gfx::Clean(probe_.error()));
            gfx::DrawWrapped(22, 146, 280, 0.38f, col::kDim, gfx::Clean(probe_.hint()), 3);
        } else if (outcome_ == ProbeOutcome::Reached && ended_) {
            gfx::DrawText(22, 132, 0.48f, col::kOn, "The room answers");
            const azl::room::RoomInfo& info = probe_.info();
            const std::string who = info.name.empty() ? std::string() : gfx::Clean(info.name) + ", ";
            gfx::DrawText(22, 148, 0.42f, col::kDim, who + std::to_string(info.members.size()) + (info.members.size() == 1 ? " player" : " players"));
        } else if (ended_) {
            gfx::DrawText(22, 132, 0.46f, col::kDim, "Leaving the room...");
        }
        const bool closable = ended_ && outcome_ != ProbeOutcome::Cancelled && probe_.leave_finished();
        DrawButton(Button(), closable ? "A" : "B", ended_ ? (closable ? "OK" : "Please wait") : "Cancel", !ended_ || closable,
                   PressedNow(g.input.in(), Button()));
        DrawFrameBorder(gfx::kBottomW);
    }

private:
    static Rect Button() {
        return Rect{84.0f, 194.0f, 152.0f, 30.0f};
    }

    void Finish(ProbeOutcome outcome) {
        ended_ = true;
        outcome_ = outcome;
        probe_.BeginLeave();
    }

    setup::Server server_;
    ProbeFn done_;
    RoomProbe probe_;
    bool ended_ = false;
    bool was_ended_ = false;
    ProbeOutcome outcome_ = ProbeOutcome::Cancelled;
};

} // namespace

ui::Page* NewProbePage(const setup::Server& server, ProbeFn done) {
    return new ProbePage(server, std::move(done));
}

} // namespace pages
