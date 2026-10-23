// The time a room of Titles Delivery still needs, worked out on the sender from what every receiver still needs. Plain C++ (no libctru, no module
// header): Tests/settings_test/eta_test.cpp builds this very file on a PC.
//
// How the sender really serves a room (ConfigApp/titleshare/common/sched.c): ONE title at a time, never in parallel; every frame is a block of the
// current title and reaches every console that lacks it. The next title is the one most receivers can take now (a small remainder of at most 7200
// blocks goes first), and a receiver that wants a title nobody else wants is served last. A receiver that joins late is another place of the room:
// when the title it wants is already queued it rides free; when it was sent before, the whole title is sent again for it alone. So the time of the
// radio is the sum over the titles of what the receiver furthest behind on each one still needs, divided by the speed; and the time of a receiver
// is when its own titles have come AND been installed. EstimateRoom does that, in the order the sender would choose.
#pragma once

#include <cstdint>

namespace delivery {

constexpr double kMiB = 1048576.0; // bytes
constexpr int kEtaTitles = 16;     // DL_MAX_TITLES
constexpr int kEtaRecv = 8;    // DL_MAX_CLIENTS

struct EtaReceiver {
    bool present = false;
    bool silent = false;   // not heard for a while: it does not count
    bool finished = false; // everything it asked for is installed
    std::uint32_t want = 0; // bit t: it asked for title t
    double down[kEtaTitles] = {}; // 0..1: how much of title t it has
    double inst[kEtaTitles] = {}; // 0..1: how much of title t is installed
    double inst_rate = 0.0;       // bytes a second its installer takes (0 = not known)
    double eff = 1.0;             // 0..1: how much of what is sent for it reaches it
};

struct EtaInput {
    int n_titles = 0;
    double size[kEtaTitles] = {}; // bytes
    int cur_title = -1;           // the title being sent now (-1 = none)
    int n_recv = 0;
    EtaReceiver recv[kEtaRecv];
    double send_rate = 0.0; // bytes a second the radio sends at, measured (0 = not known)
    double safe_rate = 0.0; // bytes a second to assume when it is not known (the safe average of the console)
};

enum class EtaKind {
    Nobody, // nobody in the room (or only receivers that have not chosen yet): `seconds` is what ONE receiver would take for all the titles
    Silent, // somebody is in but nobody answers: no time
    Done,   // everybody has everything
    Busy    // `seconds` is the time that is left
};

struct EtaReceiverOut {
    double seconds = -1.0; // when this receiver is done (-1: it is finished, silent or not there)
    double wait = 0.0;     // how long before the first title it needs starts to come
};

struct EtaOut {
    EtaKind kind = EtaKind::Nobody;
    double seconds = 0.0;
    double send_seconds = 0.0; // the time of the radio alone
    // The work of the whole room, in bytes, for the share that is done: what every receiver in it asked for, and how much of that is installed
    // (a receiver that is silent counts, one that has not chosen yet does not). `work_finished` is the part of the receivers that are finished.
    double work_total = 0.0;
    double work_done = 0.0;
    double work_finished = 0.0;
    int n_order = 0;           // the titles that still have to be sent, in the order they will be
    int order[kEtaTitles] = {};
    double start[kEtaTitles] = {}, end[kEtaTitles] = {}; // by title: when it starts and ends being sent
    EtaReceiverOut recv[kEtaRecv];
};

EtaOut EstimateRoom(const EtaInput& in);

// An average of a ratio (bytes over time) with a memory of `tau` seconds: a pause of a second moves it a little, not by half. Rate() is 0 until the
// time it has seen is at least `min_den`.
class RatioFilter {
public:
    explicit RatioFilter(double tau = 20.0) : tau_(tau) {}
    void Add(double dt, double bytes);
    double Rate(double min_den) const;
    void Reset() {
        num_ = den_ = 0.0;
    }

private:
    double tau_;
    double num_ = 0.0, den_ = 0.0;
};

// The share of the room that is done, for the percentage and the bar: what the receivers have installed over what they asked for, all of them
// together. It goes up and only goes down when somebody comes in who adds work (a late joiner): a receiver that finishes and leaves takes its work
// with it into a credit, one that leaves unfinished does not make it fall. It does not depend on the minutes (which are estimates and move).
// The count starts again after 30 seconds without any work.
class RoomShare {
public:
    // Call once a tick with the seconds since the last one; returns the percentage (0..100; 100 only when everybody is done).
    int Update(double dt, const EtaOut& eta);

private:
    double credit_ = 0.0;   // the work of the finished receivers that have left (done and asked for at once)
    double finished_ = 0.0; // the work of the finished receivers that were there at the last look
    double total_ = 0.0;    // all the work counted at the last look, the credit included
    double idle_ = 0.0;
    int pct_ = 0;
};

} // namespace delivery
