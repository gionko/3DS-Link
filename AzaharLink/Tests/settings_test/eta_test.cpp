// The time a room of Titles Delivery still needs (ConfigApp/source/delivery_eta.cpp, the very file, built on a PC): the cases of a real room
// worked out by hand from how the sender serves it (one title at a time, the one most receivers can take first, a title only one receiver wants
// last, a late joiner that wants a title that was sent before makes it count again), the filters, and the properties that must hold.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "delivery_eta.hpp"

using namespace delivery;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failures;                                                          \
            std::printf("FAILED: %s (line %d)\n", #cond, __LINE__);                \
        }                                                                          \
    } while (0)

static constexpr double kRate = 1.6 * kMiB;

static bool Near(double a, double b, double tol = 0.6) {
    return std::fabs(a - b) <= tol;
}

static EtaInput Room(std::initializer_list<double> sizes_mib, double send_rate = kRate, int cur = -1) {
    EtaInput in;
    in.n_titles = static_cast<int>(sizes_mib.size());
    int t = 0;
    for (double s : sizes_mib) {
        in.size[t++] = s * kMiB;
    }
    in.cur_title = cur;
    in.send_rate = send_rate;
    in.safe_rate = kRate;
    return in;
}

static EtaReceiver& Add(EtaInput& in, std::initializer_list<int> want) {
    EtaReceiver& r = in.recv[in.n_recv++];
    r.present = true;
    for (int t : want) {
        r.want |= 1u << t;
    }
    return r;
}

static void TestTheCases() {
    std::printf("the time of a room: the order the sender serves it in, and what each receiver waits\n");
    { // T1: six want A 1000, B 500, C 100; a seventh wants only C (wanted by all): C first, then B, then A
        EtaInput in = Room({1000, 500, 100});
        for (int i = 0; i < 6; ++i) {
            Add(in, {0, 1, 2});
        }
        Add(in, {2});
        const EtaOut o = EstimateRoom(in);
        CHECK(o.kind == EtaKind::Busy && Near(o.seconds, 1011.0));
        CHECK(o.n_order == 3 && o.order[0] == 2 && o.order[1] == 1 && o.order[2] == 0);
        CHECK(Near(o.start[2], 0.0) && Near(o.end[2], 62.5) && Near(o.start[1], 64.5) && Near(o.end[1], 377.0) && Near(o.start[0], 379.0) && Near(o.end[0], 1004.0));
        CHECK(Near(o.recv[0].seconds, 1011.0) && Near(o.recv[5].seconds, 1011.0) && Near(o.recv[6].seconds, 67.5));
        // the same with the speed not known: the safe average
        in.send_rate = 0.0;
        CHECK(Near(EstimateRoom(in).seconds, 1011.0));
    }
    { // T2: C wanted only by the seventh: it is served LAST, and the screen says how long it waits
        EtaInput in = Room({1000, 500, 100});
        for (int i = 0; i < 6; ++i) {
            Add(in, {0, 1});
        }
        Add(in, {2});
        const EtaOut o = EstimateRoom(in);
        CHECK(o.kind == EtaKind::Busy && Near(o.seconds, 1009.0));
        CHECK(o.n_order == 3 && o.order[0] == 1 && o.order[1] == 0 && o.order[2] == 2);
        CHECK(Near(o.start[2], 941.5) && Near(o.end[2], 1004.0));
        CHECK(Near(o.recv[0].seconds, 945.5) && Near(o.recv[6].seconds, 1009.0) && Near(o.recv[6].wait, 941.5));
    }
    { // T3: the same with C of 8 MiB (a remainder of at most 7200 blocks): it goes first
        EtaInput in = Room({1000, 500, 8});
        for (int i = 0; i < 6; ++i) {
            Add(in, {0, 1});
        }
        Add(in, {2});
        const EtaOut o = EstimateRoom(in);
        CHECK(Near(o.seconds, 952.5) && o.order[0] == 2 && Near(o.recv[6].seconds, 10.0) && Near(o.recv[0].seconds, 952.5));
    }
    { // T4: a late joiner wants title A (2000 MiB), sent five minutes ago to the six who have it installed; the room is on B at 40%
        EtaInput in = Room({2000, 1000}, kRate, 1);
        for (int i = 0; i < 6; ++i) {
            EtaReceiver& r = Add(in, {0, 1});
            r.down[0] = 1.0;
            r.inst[0] = 1.0;
            r.down[1] = 0.4;
            r.inst[1] = 0.35;
        }
        Add(in, {0}); // J
        const EtaOut o = EstimateRoom(in);
        CHECK(o.kind == EtaKind::Busy && Near(o.seconds, 1632.0));
        CHECK(o.n_order == 2 && o.order[0] == 1 && o.order[1] == 0 && Near(o.end[1], 375.0) && Near(o.start[0], 377.0) && Near(o.end[0], 1627.0));
        CHECK(Near(o.recv[0].seconds, 409.2) && Near(o.recv[6].seconds, 1632.0) && Near(o.recv[6].wait, 377.0));
        // T5: and if J wants B too
        in.recv[6].want |= 2u;
        CHECK(Near(EstimateRoom(in).seconds, 1928.0) && Near(EstimateRoom(in).recv[0].seconds, 675.0));
    }
    { // T6: two late joiners, one on a title that is queued (rides free) and one on a title nobody else wants
        EtaInput in = Room({1000, 500, 2000}, kRate, 0);
        for (int i = 0; i < 5; ++i) {
            EtaReceiver& r = Add(in, {0, 1});
            r.down[0] = 0.5;
            r.inst[0] = 0.45;
        }
        Add(in, {2}); // J1
        Add(in, {1}); // J2
        const EtaOut o = EstimateRoom(in);
        CHECK(Near(o.seconds, 1884.0) && o.n_order == 3 && o.order[0] == 0 && o.order[1] == 1 && o.order[2] == 2);
        CHECK(Near(o.start[0], 0.0) && Near(o.end[0], 312.5) && Near(o.start[1], 314.5) && Near(o.end[1], 627.0) && Near(o.start[2], 629.0) && Near(o.end[2], 1879.0));
        CHECK(Near(o.recv[0].seconds, 662.2) && Near(o.recv[6].seconds, 632.0) && Near(o.recv[6].wait, 314.5) && Near(o.recv[5].seconds, 1884.0) && Near(o.recv[5].wait, 629.0));
        // P1: the order of the receivers changes nothing
        EtaInput shuffled = in;
        for (int i = 0; i < 7; ++i) {
            shuffled.recv[i] = in.recv[6 - i];
        }
        const EtaOut p = EstimateRoom(shuffled);
        CHECK(Near(p.seconds, o.seconds, 1e-6) && p.n_order == o.n_order && p.order[0] == o.order[0] && p.order[2] == o.order[2]);
        CHECK(Near(p.recv[0].seconds, o.recv[6].seconds, 1e-6) && Near(p.recv[1].seconds, o.recv[5].seconds, 1e-6));
    }
    { // T7: the radio is done, one console installs slowly: the time is the installation's
        EtaInput in = Room({1000});
        for (int i = 0; i < 6; ++i) {
            in.recv[in.n_recv].finished = true;
            Add(in, {0});
        }
        EtaReceiver& x = Add(in, {0});
        x.down[0] = 1.0;
        x.inst[0] = 0.4;
        x.inst_rate = 0.4 * kMiB;
        const EtaOut o = EstimateRoom(in);
        CHECK(o.kind == EtaKind::Busy && Near(o.seconds, 1503.0) && o.n_order == 0);
    }
    { // T8: nobody; everybody done; only the silent ones left
        EtaInput in = Room({1000, 500, 100});
        EtaOut o = EstimateRoom(in);
        CHECK(o.kind == EtaKind::Nobody && Near(o.seconds, 1000.0));
        EtaReceiver& r = Add(in, {0});
        r.finished = true;
        o = EstimateRoom(in);
        CHECK(o.kind == EtaKind::Done && o.seconds == 0.0);
        EtaReceiver& q = Add(in, {1});
        q.silent = true;
        o = EstimateRoom(in);
        CHECK(o.kind == EtaKind::Silent && o.seconds < 0.0);
    }
    { // T8d: a receiver that has just come in and has not chosen yet is nobody; once the others are done the room waits for it
        EtaInput in = Room({1000, 500, 100});
        Add(in, {}); // (present, wants nothing yet)
        EtaOut o = EstimateRoom(in);
        CHECK(o.kind == EtaKind::Nobody && Near(o.seconds, 1000.0));
        in.recv[1].finished = true;
        in.recv[1].present = true;
        in.recv[1].want = 1u;
        in.n_recv = 2;
        o = EstimateRoom(in);
        CHECK(o.kind == EtaKind::Nobody);
        EtaReceiver& busy = Add(in, {0});
        busy.down[0] = 0.5;
        o = EstimateRoom(in);
        CHECK(o.kind == EtaKind::Busy && o.recv[0].seconds < 0.0 && o.recv[2].seconds > 0.0);
    }
    { // T9: a joiner far behind the pack (the catch-up costs radio time of its own)
        EtaInput in = Room({2000}, kRate, 0);
        for (int i = 0; i < 3; ++i) {
            EtaReceiver& r = Add(in, {0});
            r.down[0] = 0.5;
            r.inst[0] = 0.45;
        }
        Add(in, {0});
        const EtaOut o = EstimateRoom(in);
        CHECK(o.kind == EtaKind::Busy && Near(o.seconds, 1487.5));
    }
    { // T10: the log of a real test (C2): one done downloading, two at 78%, one new, at 1.8 MiB/s
        EtaInput in = Room({482.8}, 1.8 * kMiB, 0);
        EtaReceiver& a = Add(in, {0});
        a.down[0] = 1.0;
        a.inst[0] = 0.78;
        for (int i = 0; i < 2; ++i) {
            EtaReceiver& r = Add(in, {0});
            r.down[0] = 0.78;
            r.inst[0] = 0.78;
        }
        Add(in, {0});
        CHECK(Near(EstimateRoom(in).seconds, 306.7));
    }
    { // T11: a console that the air reaches badly (efficiency 0.7) slows the whole title down
        EtaInput in = Room({1000}, kRate, 0);
        Add(in, {0}).eff = 0.7;
        for (int i = 0; i < 6; ++i) {
            Add(in, {0});
        }
        const EtaOut o = EstimateRoom(in);
        CHECK(Near(o.seconds, 897.9) && Near(o.recv[0].seconds, 897.9));
    }
    { // T12: many small titles cost something apiece
        EtaInput in = Room({100, 100, 100, 100, 100, 100, 100, 100, 100, 100});
        Add(in, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
        CHECK(Near(EstimateRoom(in).seconds, 657.0));
    }
    { // T13: a silent console does not count, and has no time
        EtaInput in = Room({1000, 500, 100});
        for (int i = 0; i < 5; ++i) {
            Add(in, {0, 1});
        }
        Add(in, {2}).silent = true;
        const EtaOut o = EstimateRoom(in);
        CHECK(Near(o.seconds, 945.5) && o.recv[5].seconds < 0.0);
    }
}

static void TestTheProperties() {
    std::printf("the time of a room: whatever it is given, a number that makes sense\n");
    EtaInput in = Room({0, 50, 1e12}, 0.0, 7);
    in.safe_rate = 0.0;
    for (int i = 0; i < 8; ++i) {
        EtaReceiver& r = Add(in, {0, 1, 2});
        r.down[0] = 2.0;
        r.down[1] = -1.0;
        r.inst[1] = 5.0;
        r.inst[2] = std::nan("");
        r.eff = i == 0 ? 0.0 : (i == 1 ? std::nan("") : 1.0);
        r.inst_rate = i == 2 ? -3.0 : 0.0;
    }
    const EtaOut o = EstimateRoom(in);
    CHECK(o.seconds == o.seconds && o.seconds >= 0.0 && std::isfinite(o.seconds));
    for (int a = 0; a < 8; ++a) {
        CHECK(o.recv[a].seconds == o.recv[a].seconds && o.recv[a].seconds >= 0.0 && std::isfinite(o.recv[a].seconds));
    }
    // the sizes and the counts out of range do not read past the arrays
    EtaInput wild;
    wild.n_titles = 99;
    wild.n_recv = 99;
    for (int i = 0; i < kEtaRecv; ++i) {
        wild.recv[i].present = true;
        wild.recv[i].want = 0xFFFFFFFFu;
    }
    CHECK(std::isfinite(EstimateRoom(wild).seconds));
    // with the room at its biggest the order has every title once
    EtaInput full = Room({10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160});
    for (int i = 0; i < 8; ++i) {
        Add(full, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
    }
    const EtaOut f = EstimateRoom(full);
    bool seen[kEtaTitles] = {};
    for (int i = 0; i < f.n_order; ++i) {
        seen[f.order[i]] = true;
    }
    int count = 0;
    for (bool s : seen) {
        count += s ? 1 : 0;
    }
    CHECK(f.n_order == 16 && count == 16);
}

static void TestTheFilters() {
    std::printf("the filters: a rate with a memory\n");
    RatioFilter rate(20.0);
    CHECK(rate.Rate(3.0) == 0.0); // nothing seen
    for (int i = 0; i < 60; ++i) {
        rate.Add(1.0, 1.6 * kMiB);
    }
    CHECK(Near(rate.Rate(3.0) / kMiB, 1.6, 0.001));
    rate.Add(1.0, 0.0); // a second of nothing moves it a little
    CHECK(rate.Rate(3.0) / kMiB > 1.45 && rate.Rate(3.0) / kMiB < 1.6);
    RatioFilter young(20.0);
    young.Add(1.0, kMiB);
    CHECK(young.Rate(3.0) == 0.0 && young.Rate(0.5) > 0.0); // (not believed before it has seen enough time)
    young.Add(-5.0, 99.0);
    young.Add(500.0, 99.0); // (a pause of the whole app is not a measurement)
    CHECK(Near(young.Rate(0.5) / kMiB, 1.0, 0.001));
    rate.Reset();
    CHECK(rate.Rate(0.1) == 0.0);

}

// A look at the room with this much work, this much of it done and this much of it by finished receivers (the minutes do not matter to the share).
static EtaOut Worked(double total, double done, double finished = 0.0, double seconds = 600.0) {
    EtaOut o;
    o.kind = EtaKind::Busy;
    o.seconds = seconds;
    o.work_total = total;
    o.work_done = done;
    o.work_finished = finished;
    return o;
}

static void TestTheShare() {
    std::printf("the share of the room: the work done over the work asked for, it goes up and falls only when work is added\n");
    {
        RoomShare share;
        CHECK(share.Update(0.0, Worked(1000, 0)) == 0);
        CHECK(share.Update(1.0, Worked(1000, 500)) == 50);
        CHECK(share.Update(1.0, Worked(1000, 990)) == 99);
        CHECK(share.Update(1.0, Worked(1000, 1000)) == 99); // (never 100 before everybody is told to be done)
        EtaOut done;
        done.kind = EtaKind::Done;
        done.work_total = 1000;
        done.work_done = 1000;
        done.work_finished = 1000;
        CHECK(share.Update(1.0, done) == 100);
    }
    { // the minutes are estimates and move: the share does not
        RoomShare share;
        const int a = share.Update(1.0, Worked(1000, 540, 0.0, 19 * 60.0));
        const int b = share.Update(1.0, Worked(1000, 540, 0.0, 21 * 60.0));
        const int c = share.Update(1.0, Worked(1000, 540, 0.0, 20 * 60.0));
        CHECK(a == 54 && b == 54 && c == 54);
    }
    { // it never falls by itself (a data glitch, a rounding), but it does when somebody comes in who adds work
        RoomShare share;
        CHECK(share.Update(1.0, Worked(1000, 540)) == 54);
        CHECK(share.Update(1.0, Worked(1000, 530)) == 54);
        CHECK(share.Update(1.0, Worked(1500, 540)) == 36); // a late joiner asked for 500 more
        CHECK(share.Update(1.0, Worked(1500, 700)) == 46);
    }
    { // a finished receiver that leaves takes its work with it: the share stays
        RoomShare share;
        CHECK(share.Update(1.0, Worked(1000, 600, 400)) == 60);
        CHECK(share.Update(1.0, Worked(600, 200, 0)) == 60); // (400 finished, then gone: credit 400)
        CHECK(share.Update(1.0, Worked(600, 300, 0)) == 70);
    }
    { // one that leaves unfinished does not make it fall
        RoomShare share;
        CHECK(share.Update(1.0, Worked(1000, 540)) == 54);
        CHECK(share.Update(1.0, Worked(600, 300)) == 54);
        CHECK(share.Update(1.0, Worked(600, 420)) == 70);
    }
    { // nobody, or nobody answering: nothing to show, and after half a minute the count starts again
        RoomShare share;
        CHECK(share.Update(1.0, Worked(1000, 600, 400)) == 60);
        EtaOut nobody;
        nobody.kind = EtaKind::Nobody;
        CHECK(share.Update(1.0, nobody) == 0);
        for (int i = 0; i < 40; ++i) {
            share.Update(1.0, nobody);
        }
        CHECK(share.Update(1.0, Worked(1000, 200)) == 20); // (without the old credit)
    }
    { // work that is not there, or nonsense, gives a number that makes sense
        RoomShare share;
        CHECK(share.Update(1.0, Worked(0, 0)) == 0);
        const int odd = share.Update(1.0, Worked(1000, 5000, -3.0));
        CHECK(odd >= 0 && odd <= 99);
    }
    { // what EstimateRoom counts: what each receiver asked for and has installed; finished ones in full, silent ones too, one that has not chosen not at all
        EtaInput in = Room({1000, 500, 100});
        EtaReceiver& a = Add(in, {0, 1});
        a.inst[0] = 0.5;
        EtaReceiver& b = Add(in, {2});
        b.finished = true;
        EtaReceiver& c = Add(in, {0});
        c.silent = true;
        c.inst[0] = 0.1;
        Add(in, {}); // (has not chosen)
        const EtaOut o = EstimateRoom(in);
        CHECK(Near(o.work_total / kMiB, 1500.0 + 100.0 + 1000.0, 0.001));
        CHECK(Near(o.work_done / kMiB, 500.0 + 100.0 + 100.0, 0.001));
        CHECK(Near(o.work_finished / kMiB, 100.0, 0.001));
    }
}

int main() {
    TestTheCases();
    TestTheProperties();
    TestTheFilters();
    TestTheShare();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
