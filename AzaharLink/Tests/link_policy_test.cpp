// The decisions between the room link and the game's session (azl/link_policy.hpp, azl/apt_sleep.hpp), run as
// scripts: a lost link against a pause of our own network thread, the schedule of the attempts to join again and
// what the room's answers to them mean, the screen messages, the sleep notification. These are the very classes
// the plugin runs; Tests/room_integration.cpp runs them against the real room server.
#include <cstdio>

#include "azl/apt_sleep.hpp"
#include "azl/link_policy.hpp"
#include "azl/room_members.hpp"

#pragma GCC diagnostic ignored "-Wunused-function" // (support.hpp has helpers this test does not use)
#include "support.hpp"

using namespace azl;
using Error = RoomClient::Error;
using State = RoomClient::State;

static const Mac kMacA = {0x00, 0x1F, 0x32, 0x00, 0x00, 0x0A};
static const Mac kMacB = {0x00, 0x1F, 0x32, 0x00, 0x00, 0x0B};

static void TestLossGuard() {
    std::printf("loss guard: a lost link ends the session, unless our own pause caused it\n");
    LossGuard g;
    const u64 t = 100000;

    // what the room or the user ended is a loss, whatever the thread did before
    for (const Error e : {Error::RoomClosed, Error::HostKicked, Error::HostBanned, Error::None}) {
        CHECK(g.OnLeft(e, 5000, kMacA, "Nick", t) == LossGuard::Verdict::Drop);
        CHECK(!g.waiting());
    }
    // ENet gave up while the thread ran fine, or after a pause too short to be one
    CHECK(g.OnLeft(Error::LostConnection, 0, kMacA, "Nick", t) == LossGuard::Verdict::Drop);
    CHECK(g.OnLeft(Error::LostConnection, RoomClient::kStallMs - 1, kMacA, "Nick", t) == LossGuard::Verdict::Drop);
    // a pause of 2 to 10 s is put down to us
    CHECK(g.OnLeft(Error::LostConnection, RoomClient::kStallMs, kMacA, "Nick", t) == LossGuard::Verdict::Wait);
    CHECK(g.waiting());
    CHECK(g.OnLeft(Error::LostConnection, LossGuard::kLongStallMs, kMacA, "Nick", t) == LossGuard::Verdict::Wait);
    // ... a longer one is not: the other players gave up on us long ago
    CHECK(g.OnLeft(Error::LostConnection, LossGuard::kLongStallMs + 1, kMacA, "Nick", t) == LossGuard::Verdict::Drop);
    CHECK(!g.waiting());
    // the room ended it anyway
    CHECK(g.OnLeft(Error::LostConnection, 6000, kMacA, "Nick", t) == LossGuard::Verdict::Wait);
    CHECK(g.OnLeft(Error::RoomClosed, 6000, kMacA, "Nick", t) == LossGuard::Verdict::Drop);
    CHECK(!g.waiting());

    // the same member back in time: the session goes on, once
    CHECK(g.OnJoined(kMacA, "Nick", t) == LossGuard::Outcome::Nothing);
    CHECK(g.OnLeft(Error::LostConnection, 6000, kMacA, "Nick", t) == LossGuard::Verdict::Wait);
    CHECK(!g.Due(t + LossGuard::kRejoinMs));
    CHECK(g.OnJoined(kMacA, "Nick", t + 9000) == LossGuard::Outcome::Kept);
    CHECK(!g.waiting());
    CHECK(g.OnJoined(kMacA, "Nick", t + 9001) == LossGuard::Outcome::Nothing);
    // another MAC, another name, or too late: the session ends
    CHECK(g.OnLeft(Error::LostConnection, 6000, kMacA, "Nick", t) == LossGuard::Verdict::Wait);
    CHECK(g.OnJoined(kMacB, "Nick", t + 1000) == LossGuard::Outcome::Drop);
    CHECK(g.OnLeft(Error::LostConnection, 6000, kMacA, "Nick", t) == LossGuard::Verdict::Wait);
    CHECK(g.OnJoined(kMacA, "Nick-0B0A", t + 1000) == LossGuard::Outcome::Drop);
    CHECK(g.OnLeft(Error::LostConnection, 6000, kMacA, "Nick", t) == LossGuard::Verdict::Wait);
    CHECK(g.OnJoined(kMacA, "Nick", t + LossGuard::kRejoinMs + 1) == LossGuard::Outcome::Drop);
    CHECK(!g.waiting());

    // the room does not take us back in time: Due() says so once
    CHECK(g.OnLeft(Error::LostConnection, 6000, kMacA, "Nick", t) == LossGuard::Verdict::Wait);
    CHECK(!g.Due(t + 14999));
    CHECK(g.Due(t + LossGuard::kRejoinMs + 1));
    CHECK(!g.Due(t + 99999));
    CHECK(!g.waiting());
    // a wait that is ended by hand (the user switched the connection off)
    CHECK(g.OnLeft(Error::LostConnection, 6000, kMacA, "Nick", t) == LossGuard::Verdict::Wait);
    CHECK(g.Settle());
    CHECK(!g.Settle());
    CHECK(!g.Due(t + 99999));
    // a second loss starts its own clock
    CHECK(g.OnLeft(Error::LostConnection, 6000, kMacA, "Nick", t + 50000) == LossGuard::Verdict::Wait);
    CHECK(!g.Due(t + 50000 + 14000));
}

namespace {

/// Runs the policy the way the plugin's Tick does, once per call.
struct Script {
    RejoinPolicy policy;
    u64 launch = 0;
    bool want = true;
    bool automatic = true;
    u8 wifi = RejoinPolicy::kWifiConnected; ///< the network state byte of the shared page, as the test sets it

    RejoinPolicy::Step At(u64 now, State state, Error error) {
        RejoinPolicy::Input in;
        in.want_connected = want;
        in.state = state;
        in.error = error;
        in.now_ms = now;
        in.launch_ms = launch;
        in.nickname_automatic = automatic;
        in.wifi_state = wifi;
        return policy.Update(in);
    }
};

} // namespace

static void TestRejoinSchedule() {
    std::printf("rejoin policy: a failed attempt is followed by the next one 2 s after it began, always\n");
    Script s;
    s.launch = 0;
    // attempts at once, then every 2 s: a failed attempt leaves the client idle with its error (no growing interval:
    // 0.1.15 waited 2, 4, 6, 8 and then 10 s)
    const u64 base = 1000000;
    u32 attempts = 0;
    u64 last = 0;
    for (u64 dt = 0; dt <= 60000; dt += 100) {
        const RejoinPolicy::Step step = s.At(base + dt, State::Idle, Error::CouldNotConnect);
        CHECK(step.alive);
        CHECK(step.wifi == RejoinPolicy::WifiNote::None);
        if (step.connect) {
            CHECK(attempts == 0 ? dt == 0 : dt - last == RejoinPolicy::kRetryMs);
            last = dt;
            ++attempts;
            CHECK_EQ(s.policy.attempts(), attempts);
        }
    }
    CHECK_EQ(attempts, 31u); // 0, 2, 4 ... 60 s
    CHECK_EQ(s.policy.NextAttemptInMs(base + 60000 + 500), 1500u);
    // once the link is healthy it starts over, and the number of attempts too
    s.At(base + 70000, State::Joined, Error::None);
    CHECK_EQ(s.policy.attempts(), 0u);
}

static void TestRejoinAfterLoss() {
    std::printf("rejoin policy: our own old link is still listed for a while: wait, do not change who we are\n");
    Script s;
    s.launch = 0;
    const u64 base = 1000000; // (long after the launch: no launch grace)
    s.At(base - 1000, State::Joined, Error::None);
    s.policy.NoteJoined();
    s.policy.NoteLoss(base);

    // the first attempt is at once (the link was healthy for long), the room says the old member is there
    u32 attempts = 0, said = 0;
    u64 times[32] = {};
    for (u64 dt = 0; dt < 35000; dt += 100) {
        const RejoinPolicy::Step step = s.At(base + dt, State::Idle, dt == 0 ? Error::LostConnection : Error::NameCollision);
        CHECK(step.alive);
        said += step.note == RejoinPolicy::Note::OldLink;
        CHECK(step.note != RejoinPolicy::Note::NameTaken && step.note != RejoinPolicy::Note::MacTaken);
        if (step.connect && attempts < 32) {
            times[attempts++] = dt;
        }
    }
    CHECK_EQ(said, 1u);
    CHECK(s.policy.prefer_real_mac());       // no identity flip
    CHECK(!s.policy.nickname_suffixed());
    CHECK(!s.policy.gave_up());
    CHECK_EQ(attempts, 18u);                  // 0, 2, 4 ... 34 s: every 2 s through the 35 s the old link may be listed
    for (u32 i = 0; i < 18; ++i) {
        CHECK_EQ(times[i], 2000u * i);
    }
    CHECK(s.policy.InOldLinkWindow(base + 34999));
    CHECK(!s.policy.InOldLinkWindow(base + 35000));

    // every error the room gives for a member that is already there is waited out inside the window ...
    for (const Error e : {Error::NameCollision, Error::MacCollision, Error::ConsoleIdCollision, Error::RoomIsFull}) {
        Script w;
        w.At(base - 1000, State::Joined, Error::None);
        w.policy.NoteLoss(base);
        const RejoinPolicy::Step step = w.At(base + 20000, State::Idle, e);
        CHECK(step.alive && step.connect);
        CHECK(w.policy.prefer_real_mac() && !w.policy.nickname_suffixed() && !w.policy.gave_up());
    }
    // ... but one that has nothing to do with it is not
    for (const Error e : {Error::WrongPassword, Error::WrongVersion, Error::HostBanned, Error::HostKicked}) {
        Script w;
        w.At(base - 1000, State::Joined, Error::None);
        w.policy.NoteLoss(base);
        CHECK(!w.At(base + 1000, State::Idle, e).alive);
        CHECK(w.policy.gave_up());
    }
    // after the window the old rules apply: the identity changes once, then it is over
    {
        Script w;
        w.At(base - 1000, State::Joined, Error::None);
        w.policy.NoteLoss(base);
        RejoinPolicy::Step step = w.At(base + 36000, State::Idle, Error::MacCollision);
        CHECK(step.alive && step.note == RejoinPolicy::Note::MacTaken && step.connect);
        CHECK(!w.policy.prefer_real_mac());
        step = w.At(base + 36100, State::Idle, Error::NameCollision);
        CHECK(step.alive && step.note == RejoinPolicy::Note::NameTaken);
        CHECK(w.policy.nickname_suffixed());
        step = w.At(base + 36200, State::Idle, Error::ConsoleIdCollision);
        CHECK(!step.alive);
        CHECK(w.policy.gave_up());
        CHECK(!w.At(base + 36300, State::Idle, Error::None).alive); // (stays given up)
    }
    // a nickname the user chose is not suffixed: the collision is fatal after the window
    {
        Script w;
        w.automatic = false;
        w.At(base - 1000, State::Joined, Error::None);
        w.policy.NoteLoss(base);
        CHECK(!w.At(base + 40000, State::Idle, Error::NameCollision).alive);
    }
    // a join ends the window
    {
        Script w;
        w.At(base - 1000, State::Joined, Error::None);
        w.policy.NoteLoss(base);
        w.policy.NoteJoined();
        CHECK(!w.policy.InOldLinkWindow(base + 1000));
        CHECK(!w.At(base + 1000, State::Idle, Error::ConsoleIdCollision).alive);
    }
}

static void TestRejoinLaunch() {
    std::printf("rejoin policy: right after the launch the room may still list this console from the game before\n");
    const u64 launch = 5000;
    Script s;
    s.launch = launch;
    // MAC taken: the room-assigned one at once; a nickname from the console is suffixed; then collisions are retried
    // for 45 s from the launch and fatal after that
    RejoinPolicy::Step step = s.At(launch + 1000, State::Idle, Error::MacCollision);
    CHECK(step.alive && step.connect && step.note == RejoinPolicy::Note::MacTaken);
    step = s.At(launch + 1100, State::Idle, Error::NameCollision);
    CHECK(step.alive && step.connect && step.note == RejoinPolicy::Note::NameTaken);
    step = s.At(launch + 1200, State::Idle, Error::ConsoleIdCollision);
    CHECK(step.alive); // (the schedule decides when)
    step = s.At(launch + 44000, State::Idle, Error::ConsoleIdCollision);
    CHECK(step.alive && step.connect);
    CHECK(!s.At(launch + 45000, State::Idle, Error::ConsoleIdCollision).alive);
    CHECK(s.policy.gave_up());
    // the room being full is fatal, also in the grace time (as it always was)
    Script f;
    f.launch = launch;
    CHECK(!f.At(launch + 1000, State::Idle, Error::RoomIsFull).alive);
}

static void TestRejoinUser() {
    std::printf("rejoin policy: the user's switch, the network layer that will not start\n");
    Script s;
    s.launch = 0;
    const u64 t = 1000000;
    // switched off: a client that is not idle is stopped; nothing is tried
    s.want = false;
    RejoinPolicy::Step step = s.At(t, State::Joined, Error::None);
    CHECK(step.disconnect && !step.connect && step.alive);
    step = s.At(t + 10, State::Idle, Error::LostConnection);
    CHECK(!step.disconnect && !step.connect && step.alive);
    // switched on again: the last error is forgotten and the attempt is at once
    s.want = true;
    step = s.At(t + 5000, State::Idle, Error::LostConnection);
    CHECK(step.clear_error && step.connect && step.alive);
    // a fatal error given up on is forgotten by switching off and on
    CHECK(!s.At(t + 6000, State::Idle, Error::WrongPassword).alive);
    s.want = false;
    CHECK(s.At(t + 7000, State::Idle, Error::WrongPassword).alive);
    CHECK(!s.policy.gave_up());
    s.want = true;
    step = s.At(t + 8000, State::Idle, Error::WrongPassword);
    CHECK(step.clear_error && step.alive && step.connect);

    // the network layer that will not start: three tries, then it is over; no address: at once
    Script n;
    n.launch = 0;
    CHECK(n.At(t, State::Idle, Error::None).connect);
    CHECK(n.policy.AttemptFailed(true));
    CHECK_EQ(n.policy.failures(), 1u);
    CHECK(n.At(t + 2000, State::Idle, Error::None).connect);
    CHECK(n.policy.AttemptFailed(true));
    CHECK(n.At(t + 4000, State::Idle, Error::None).connect);
    CHECK(!n.policy.AttemptFailed(true));
    CHECK(n.policy.gave_up());
    CHECK(!n.At(t + 12000, State::Idle, Error::None).alive);
    Script h;
    CHECK(h.At(t, State::Idle, Error::None).connect);
    CHECK(!h.policy.AttemptFailed(false));
    // sockets that came up reset the count
    Script r;
    CHECK(r.At(t, State::Idle, Error::None).connect);
    CHECK(r.policy.AttemptFailed(true));
    r.policy.NetworkReady();
    CHECK_EQ(r.policy.failures(), 0u);

    // a wake-up: the next attempt is at once and the schedule starts over
    Script w;
    w.At(t, State::Idle, Error::CouldNotConnect);
    w.At(t + 2000, State::Idle, Error::CouldNotConnect);
    w.At(t + 4000, State::Idle, Error::CouldNotConnect);
    CHECK(!w.At(t + 4500, State::Idle, Error::CouldNotConnect).connect);
    w.policy.ResetBackoff();
    CHECK(w.At(t + 4600, State::Idle, Error::CouldNotConnect).connect);
    CHECK(!w.At(t + 6000, State::Idle, Error::CouldNotConnect).connect);
    CHECK(w.At(t + 6600, State::Idle, Error::CouldNotConnect).connect);
}

// The console's WiFi (the network state byte of the shared page): while it is down no attempt is made, the byte is read
// every 500 ms, and the moment it says "connected" again the attempt is at once (spec: PROVA 1 wasted 3.4 s on average
// after the WiFi was back and made 16 failing attempts out of 17 while it was down).
static void TestRejoinWifi() {
    std::printf("rejoin policy: no attempt while the WiFi is down, one at once when it is back, then every 2 s\n");
    const u64 t = 1000000;
    {
        // WiFi up and down: attempts run until the byte is looked at, none while it is down, one the instant it is back
        Script s;
        s.launch = 0;
        s.At(t, State::Joined, Error::None); // (seen connected)
        u32 connects_while_down = 0, down_notes = 0, back_notes = 0;
        u64 down_noted_at = 0, back_noted_at = 0, first_attempt_after = 0;
        s.wifi = 1;                          // the access point is gone (0 off, 1 associating, 5 the back-off: all "down")
        const u64 dropped = t + 100;
        bool seen_down = false;
        for (u64 dt = 100; dt <= 20000; dt += 50) {
            const RejoinPolicy::Step step = s.At(t + dt, State::Idle, Error::LostConnection);
            if (step.wifi == RejoinPolicy::WifiNote::Down) {
                ++down_notes;
                down_noted_at = t + dt;
                seen_down = true;
                CHECK_EQ(step.wifi_state, 1u);
            }
            if (seen_down && step.connect) {
                ++connects_while_down;
            }
            CHECK(step.alive);
        }
        CHECK_EQ(down_notes, 1u);             // said once
        CHECK_EQ(connects_while_down, 0u);    // 20 s of silence
        CHECK(s.policy.waiting_for_wifi());
        CHECK(down_noted_at - dropped <= RejoinPolicy::kWifiPollMs + 50);   // looked at within 500 ms
        // back: the next look finds it, the attempt is in the same call
        s.wifi = RejoinPolicy::kWifiConnected;
        const u64 back = t + 20000;
        for (u64 dt = 0; dt <= 7000; dt += 50) {
            const RejoinPolicy::Step step = s.At(back + dt, State::Idle, Error::NetworkUnavailable);
            if (step.wifi == RejoinPolicy::WifiNote::Back) {
                ++back_notes;
                back_noted_at = back + dt;
                CHECK(step.connect);          // at once
                CHECK(step.wifi_down_ms >= 19000 && step.wifi_down_ms <= 20000);
            } else if (step.connect && first_attempt_after == 0) {
                first_attempt_after = back + dt;
            }
        }
        CHECK_EQ(back_notes, 1u);
        CHECK(back_noted_at - back <= RejoinPolicy::kWifiPollMs);              // within one look
        CHECK(!s.policy.waiting_for_wifi());
        CHECK_EQ(first_attempt_after - back_noted_at, 2000u);                   // the retry is 2 s after that one
    }
    {
        // the retry every 2 s goes on while the attempts keep failing with the WiFi up, and stops at once when it drops again
        Script s;
        s.At(t, State::Joined, Error::None);
        u64 last = 0;
        u32 attempts = 0;
        for (u64 dt = 0; dt < 6000; dt += 100) {
            const RejoinPolicy::Step step = s.At(t + dt, State::Idle, Error::CouldNotConnect);
            if (step.connect) {
                CHECK(attempts == 0 || t + dt - last == 2000);
                last = t + dt;
                ++attempts;
            }
        }
        CHECK_EQ(attempts, 3u);
        s.wifi = 0;
        u32 more = 0;
        for (u64 dt = 6000; dt < 30000; dt += 100) {
            more += s.At(t + dt, State::Idle, Error::CouldNotConnect).connect ? 1 : 0;
        }
        CHECK(more <= 1);                    // (at most the one that was due before the byte was looked at)
        s.wifi = 2;
        CHECK(s.At(t + 30000, State::Idle, Error::CouldNotConnect).connect);   // back: at once
    }
    {
        // terminal errors do not retry, WiFi or not; the user's switch ends everything
        for (const Error e : {Error::WrongPassword, Error::WrongVersion, Error::HostKicked, Error::HostBanned}) {
            Script s;
            s.At(t, State::Joined, Error::None);
            s.wifi = 0;
            s.At(t + 1000, State::Idle, Error::None);          // (waiting for the WiFi)
            s.wifi = 2;
            const RejoinPolicy::Step step = s.At(t + 5000, State::Idle, e);
            CHECK(!step.alive && !step.connect);                // no attempt now that it is back
            CHECK(s.policy.gave_up());
        }
        Script s;
        s.At(t, State::Joined, Error::None);
        s.wifi = 0;
        s.At(t + 1000, State::Idle, Error::LostConnection);
        s.At(t + 1600, State::Idle, Error::LostConnection);
        CHECK(s.policy.waiting_for_wifi());
        s.want = false;                                         // "Connect to room" off: nothing is waited for
        s.At(t + 2000, State::Idle, Error::LostConnection);
        CHECK(!s.policy.waiting_for_wifi());
        s.wifi = 2;
        s.want = false;
        CHECK(!s.At(t + 5000, State::Idle, Error::LostConnection).connect);
    }
    {
        // a byte that never read "connected" holds nothing back (a console where it means something else is not locked out),
        // and gates from the moment it reads it
        Script s;
        s.wifi = 0;
        u32 attempts = 0;
        for (u64 dt = 0; dt < 10000; dt += 100) {
            attempts += s.At(t + dt, State::Idle, Error::NetworkUnavailable).connect ? 1 : 0;
        }
        CHECK_EQ(attempts, 5u);              // 0, 2, 4, 6, 8 s as if the byte was not there
        CHECK(!s.policy.waiting_for_wifi());
        s.wifi = 2;
        s.At(t + 10000, State::Idle, Error::NetworkUnavailable);
        s.wifi = 5;
        u32 later = 0;
        for (u64 dt = 10100; dt < 30000; dt += 100) {
            later += s.At(t + dt, State::Idle, Error::NetworkUnavailable).connect ? 1 : 0;
        }
        CHECK(later <= 1);                   // now it is known to mean something
        CHECK(s.policy.waiting_for_wifi());
    }
    {
        // the byte is read every 500 ms: a dip between two looks is not seen
        Script s;
        s.At(t, State::Idle, Error::CouldNotConnect);          // look at t (the first attempt)
        s.wifi = 0;
        const RejoinPolicy::Step early = s.At(t + 100, State::Idle, Error::CouldNotConnect);
        CHECK(early.wifi == RejoinPolicy::WifiNote::None && !s.policy.waiting_for_wifi()); // (not looked at yet)
        s.wifi = 2;
        s.At(t + 400, State::Idle, Error::CouldNotConnect);
        s.At(t + 600, State::Idle, Error::CouldNotConnect);
        CHECK(!s.policy.waiting_for_wifi());
        // a drop that is still there at the next look is seen, and only then: not at t+1000 (the look at t+600 saw 2), at t+1100
        s.wifi = 0;
        const RejoinPolicy::Step between = s.At(t + 1000, State::Idle, Error::CouldNotConnect);
        CHECK(between.wifi == RejoinPolicy::WifiNote::None && !s.policy.waiting_for_wifi());
        const RejoinPolicy::Step seen = s.At(t + 1100, State::Idle, Error::CouldNotConnect);
        CHECK(seen.wifi == RejoinPolicy::WifiNote::Down && s.policy.waiting_for_wifi());
        s.wifi = 2;
        const RejoinPolicy::Step back = s.At(t + 1700, State::Idle, Error::CouldNotConnect); // (the look after that one)
        CHECK(back.wifi == RejoinPolicy::WifiNote::Back && !s.policy.waiting_for_wifi());
        // ... and the wake-up reads it again at once
        s.At(t + 2000, State::Idle, Error::CouldNotConnect);   // (look at t+1700 .. next at t+2200)
        s.wifi = 0;
        s.policy.ResetBackoff();
        const RejoinPolicy::Step step = s.At(t + 2050, State::Idle, Error::CouldNotConnect);
        CHECK(step.wifi == RejoinPolicy::WifiNote::Down && !step.connect);
    }
}

static void TestAnnouncer() {
    std::printf("announcer: at most one message every 5 s, the newest one that came too soon is kept\n");
    Announcer a;
    CHECK(a.Offer("first", 1000) == "first");
    CHECK(a.Offer("second", 3000).empty());
    CHECK(a.Poll(5999).empty());
    CHECK(a.Offer("third", 4000).empty());      // replaces the second
    CHECK(a.Poll(6000) == "third");
    CHECK(a.Poll(6001).empty());                 // (once)
    CHECK(a.Offer("fourth", 8000).empty());      // 2 s after the last one
    CHECK(a.Poll(11000) == "fourth");
    CHECK(a.Offer("fifth", 30000) == "fifth");   // a quiet time: at once
    CHECK(a.Poll(60000).empty());
}

static void TestSleepWatch() {
    std::printf("sleep watch: the notifications, the framework's events, a sleep that was missed, one that never came\n");
    {
        // the game's notifications: sleep begins, then wake-up; the framework's events say the same afterwards
        SleepWatch w;
        CHECK(!w.sleeping());
        w.Enter(1000, true);
        CHECK(w.sleeping());
        SleepWatch::Result r = w.Update(1100, 1, 0, true);
        CHECK(!r.woke && !r.missed && !r.timed_out && w.sleeping());
        w.Woke();
        r = w.Update(1200, 1, 0, true);
        CHECK(r.apt_wake && r.woke && !w.sleeping());
        r = w.Update(1300, 1, 1, true);        // (the same wake-up, by the other road: nothing to do twice)
        CHECK(r.fw_wake && !r.woke && !r.missed);
        r = w.Update(1400, 1, 1, true);
        CHECK(!r.apt_wake && !r.fw_wake && !r.woke);
    }
    {
        // the framework's wake-up first
        SleepWatch w;
        w.Enter(1000, true);
        SleepWatch::Result r = w.Update(1100, 1, 1, true);
        CHECK(r.fw_wake && r.woke && !r.missed && !w.sleeping());
        w.Woke();
        r = w.Update(1200, 1, 1, true);
        CHECK(r.apt_wake && !r.woke);
    }
    {
        // sleep_stop off: sleeps are counted and nothing is tracked
        SleepWatch w;
        w.Enter(1000, false);
        CHECK(!w.sleeping());
        SleepWatch::Result r = w.Update(1100, 1, 1, false);
        CHECK(r.fw_wake && !r.woke && !r.missed);
    }
    {
        // the game's notifications are not seen at all: the framework's enter and exit say the console slept
        SleepWatch w;
        SleepWatch::Result r = w.Update(5000, 1, 0, true);
        CHECK(!r.woke && !r.missed);
        r = w.Update(6000, 1, 1, true);
        CHECK(r.fw_wake && r.missed && r.woke && !w.sleeping());
        r = w.Update(7000, 2, 2, true);       // a second sleep the same way
        CHECK(r.missed && r.woke);
        // ... and one the game did show is not missed
        w.Enter(8000, true);
        w.Woke();
        r = w.Update(9000, 3, 3, true);
        CHECK(r.apt_wake && r.fw_wake && r.woke && !r.missed);
    }
    {
        // a notification of a sleep that never happened: after 30 s awake it is over
        SleepWatch w;
        w.Enter(1000, true);
        SleepWatch::Result r = w.Update(1000 + SleepWatch::kWakeWaitMs, 1, 0, true);
        CHECK(!r.timed_out && !r.woke && w.sleeping());
        r = w.Update(1001 + SleepWatch::kWakeWaitMs, 1, 0, true);
        CHECK(r.timed_out && r.woke && !w.sleeping());
    }
    {
        // `acts` is only asked for when the framework reported a wake-up that Update() has not seen (with sleep_stop=1 working
        // it out takes the engine's lock: not on every frame)
        SleepWatch w;
        CHECK(!w.NeedsActs(0));            // (the frame callback's every frame)
        CHECK(w.NeedsActs(1));             // a SLEEP_EXIT the watch has not looked at
        (void)w.Update(1000, 1, 1, false);
        CHECK(!w.NeedsActs(1));            // looked at
        CHECK(w.NeedsActs(2));
    }
}

static void TestAptSleepEvents() {
    std::printf("apt: the sleep notifications, and nothing else\n");
    // InquireNotification replies: [0] header, [1] result, [2] the notification
    const u32 enter[] = {0x000B0080, 0, 5}, wake[] = {0x000B0080, 0, 6}, query[] = {0x000B0080, 0, 3},
              cancel[] = {0x000B0080, 0, 4}, none[] = {0x000B0080, 0, 0}, home[] = {0x000B0080, 0, 1},
              failed[] = {0x000B0080, 0xD8A0CC0B, 5};
    CHECK(AptSleepEventOf(0x000B0040, enter) == AptSleepEvent::Enter);
    CHECK(AptSleepEventOf(0x000B0040, wake) == AptSleepEvent::Wake);
    CHECK(AptSleepEventOf(0x000B0040, query) == AptSleepEvent::None);   // the apps may still refuse
    CHECK(AptSleepEventOf(0x000B0040, cancel) == AptSleepEvent::None);
    CHECK(AptSleepEventOf(0x000B0040, none) == AptSleepEvent::None);
    CHECK(AptSleepEventOf(0x000B0040, home) == AptSleepEvent::None);
    CHECK(AptSleepEventOf(0x000B0040, failed) == AptSleepEvent::None);
    // bits above the signal (Mario Party: The Top 100 got 0x1BC00001 for the HOME button on a console, 2026-10-14): the signal is the low 16 bits
    const u32 enter_hi[] = {0x000B0080, 0, 0x1BC00005}, wake_hi[] = {0x000B0080, 0, 0x1BC00006}, home_hi[] = {0x000B0080, 0, 0x1BC00001},
              only_hi[] = {0x000B0080, 0, 0x1BC00000};
    CHECK(AptSleepEventOf(0x000B0040, enter_hi) == AptSleepEvent::Enter);
    CHECK(AptSleepEventOf(0x000B0040, wake_hi) == AptSleepEvent::Wake);
    CHECK(AptSleepEventOf(0x000B0040, home_hi) == AptSleepEvent::None);
    CHECK(AptSleepEventOf(0x000B0040, only_hi) == AptSleepEvent::None);
    CHECK_EQ(AptSignalOf(0x1BC00001), 1u);
    CHECK_EQ(AptSignalOf(5), 5u);
    CHECK_EQ(AptSignalOf(0xFFFF0006), 6u);
    // the other calls of the sleep protocol carry the same words in other places and mean nothing here
    CHECK(AptSleepEventOf(0x003E0080, enter) == AptSleepEvent::None);   // ReplySleepQuery (accept = 1, reject = 0)
    CHECK(AptSleepEventOf(0x003F0040, enter) == AptSleepEvent::None);   // ReplySleepNotificationComplete
    CHECK(AptSleepEventOf(0x00010040, wake) == AptSleepEvent::None);
    CHECK(AptSleepEventOf(0, enter) == AptSleepEvent::None);
}

static void TestSleepAction() {
    std::printf("sleep action: sleep_stop 0 never acts, 1 for a console that is not hosting, 2 for a host too\n");
    for (const bool hosting : {false, true}) {
        CHECK(!SleepActsFor(0, hosting));
        CHECK(SleepActsFor(2, hosting));
    }
    CHECK(SleepActsFor(1, false));   // a client, or a console in no session
    CHECK(!SleepActsFor(1, true));   // a host keeps its lobby: it is the role that froze the console in PROVA 1
}

// The lines of the log for the players that come into the room and leave it (the plugin's Plugin::NoteRoomMembers writes them)
static void TestRoomMemberLines() {
    std::printf("room members: a line for every player that came in and every one that left, by MAC\n");
    const Mac mac_c = {0x00, 0x1F, 0x32, 0xAB, 0xCD, 0xEF};
    const RoomMemberRow a{kMacA, "Friend-A", "FINAL FANTASY EXPLORERS"};
    const RoomMemberRow b{kMacB, "Friend-D", "FINAL FANTASY EXPLORERS"};
    const RoomMemberRow c{mac_c, "Gionko", ""};
    // nobody came or left: no line, whatever else the list says (a nickname or a title can change under the same MAC)
    CHECK(RoomMemberLines({a, b}, {a, b}, 8).empty());
    CHECK(RoomMemberLines({}, {}, 8).empty());
    {
        RoomMemberRow renamed = b;
        renamed.nickname = "FriendD2";
        renamed.game = "";
        CHECK(RoomMemberLines({a, b}, {a, renamed}, 8).empty());
        CHECK(RoomMemberLines({a, b}, {b, a}, 8).empty()); // (the order of the list is not a change)
    }
    // one comes in: the name, the title the room knows, the MAC the frames are told apart by, how many there are now
    std::vector<std::string> lines = RoomMemberLines({a, b}, {a, b, c}, 8);
    CHECK_EQ(lines.size(), std::size_t(1));
    CHECK(lines.size() == 1 && lines[0] == "room: 'Gionko' joined the room (MAC 00:1F:32:AB:CD:EF): 3/8 players");
    lines = RoomMemberLines({a}, {a, b}, 4);
    CHECK(lines.size() == 1 && lines[0] == "room: 'Friend-D' [FINAL FANTASY EXPLORERS] joined the room (MAC 00:1F:32:00:00:0B): 2/4 players");
    // one leaves
    lines = RoomMemberLines({a, b, c}, {a, c}, 8);
    CHECK(lines.size() == 1 && lines[0] == "room: 'Friend-D' [FINAL FANTASY EXPLORERS] left the room (MAC 00:1F:32:00:00:0B): 2/8 players");
    // two change places in one list: the ones that came first, then the ones that left, each in the order of its list
    lines = RoomMemberLines({a, b}, {c, a}, 8);
    CHECK_EQ(lines.size(), std::size_t(2));
    CHECK(lines.size() == 2 && lines[0] == "room: 'Gionko' joined the room (MAC 00:1F:32:AB:CD:EF): 2/8 players");
    CHECK(lines.size() == 2 && lines[1] == "room: 'Friend-D' [FINAL FANTASY EXPLORERS] left the room (MAC 00:1F:32:00:00:0B): 2/8 players");
    // everybody gone, and everybody there at once
    CHECK_EQ(RoomMemberLines({a, b, c}, {}, 8).size(), std::size_t(3));
    CHECK_EQ(RoomMemberLines({}, {a, b, c}, 8).size(), std::size_t(3));
    // a nickname that looks like the rest of the line is still one line
    lines = RoomMemberLines({}, {RoomMemberRow{kMacA, "a' left the room (MAC", ""}}, 8);
    CHECK(lines.size() == 1 && lines[0] == "room: 'a' left the room (MAC' joined the room (MAC 00:1F:32:00:00:0A): 1/8 players");
}

int main() {
    TestRoomMemberLines();
    TestLossGuard();
    TestRejoinSchedule();
    TestRejoinAfterLoss();
    TestRejoinLaunch();
    TestRejoinUser();
    TestRejoinWifi();
    TestAnnouncer();
    TestSleepWatch();
    TestAptSleepEvents();
    TestSleepAction();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
