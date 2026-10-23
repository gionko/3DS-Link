// Host-side tests of the receive path of a bind: the byte ring (uds_ring.hpp), the bind table of the engine
// (lookup, 16 entries, admission against the game's UDS block, events, counters), the binds that survive the
// end of a session while their queued frames do not, and the load the ring has to take.
//   1. RxRing against a model of nwm's ring (a port of the model the firmware analysis checked)
//   2. the engine's bind table, through the mock room
//   3. binds survive Disconnect / Destroy (spec C3): the queued frames do not
//   4. the FFE exit race: Unbind and PullPacket on two threads
//   5. the load model: real FFE frame sizes and arrival times with a game that stops pulling (spec C2)
// Build & run: Tests/run.sh
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "azl/stats.hpp"
#include "azl/uds_ring.hpp"
#include "mock_room.hpp"
#include "rx_load_profile.inc"

// ---------------------------------------------------------------------------------------------
// 1. The ring
// ---------------------------------------------------------------------------------------------

// What the firmware does, in the plainest form: entries in a list, head and tail in units (the model of
// the analysis, spec/dp/ringmodel.py, in C++).
struct RingModel {
    struct Entry {
        u32 start, units, n, tag;
    };
    explicit RingModel(u32 bytes) : total(bytes / 32) {}
    u32 Free() const {
        if (head == tail) {
            return total;
        }
        return head > tail ? head - tail : total - (tail - head);
    }
    void DropOldest() {
        const Entry e = entries.front();
        entries.erase(entries.begin());
        ++over;
        head = (head + e.units) % total;
    }
    // 0 stored, 1 nothing to store, 2 refused
    int Enqueue(u32 n, u32 tag) {
        if (n > 0x5D4 - 10) {
            return 2;
        }
        if (n == 0) {
            return 1;
        }
        const u32 units = (n + 8 + 31) / 32;
        while (units > Free()) {
            DropOldest();
        }
        entries.push_back({tail, units, n, tag});
        tail = (tail + units) % total;
        ++enq;
        if (tail == head) {
            DropOldest();
        }
        return 0;
    }
    bool Pull(u32* n, u32* tag) {
        if (entries.empty()) {
            return false;
        }
        const Entry e = entries.front();
        entries.erase(entries.begin());
        head = (head + e.units) % total;
        *n = e.n;
        *tag = e.tag;
        return true;
    }
    u32 Stored() const {
        u32 s = 0;
        for (const Entry& e : entries) {
            s += e.units;
        }
        return s;
    }
    u32 total, head = 0, tail = 0, enq = 0, over = 0;
    std::vector<Entry> entries;
};

static std::vector<u8> Pattern(u32 tag, u32 n) {
    std::vector<u8> v(n);
    for (u32 i = 0; i < n; ++i) {
        v[i] = static_cast<u8>(tag * 31 + i * 7);
    }
    return v;
}

static void TestRingAgainstModel() {
    std::printf("ring: against the model of nwm's ring\n");
    unsigned seed = 12345;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    const u32 sizes[] = {0x5F4, 0x646, 0x1000, 0x2E30, 0x4000};
    const u32 payloads[] = {1, 20, 73, 140, 267, 700, 1478, 1482, 1483, 0};
    for (u32 bytes : sizes) {
        RxRing ring;
        CHECK(ring.Init(bytes));
        RingModel model(bytes);
        CHECK_EQ(ring.total_units(), bytes / 32);
        u32 refused = 0, nothing = 0;
        for (int step = 0; step < 60000; ++step) {
            if (rnd() % 100 < 62) {
                const u32 n = rnd() % 4 == 0 ? rnd() % 1500 : payloads[rnd() % 10];
                const std::vector<u8> data = Pattern(step, n);
                const int want = model.Enqueue(n, step);
                const RxRing::Put got = ring.Enqueue(1, static_cast<u16>(step), 7, data.data(), n, static_cast<u32>(step) + 1);
                CHECK_EQ(static_cast<int>(got), want);
                refused += want == 2;
                nothing += want == 1;
            } else {
                u32 n = 0, tag = 0;
                const bool had = model.Pull(&n, &tag);
                u8 out[1600];
                u32 got_n = 0;
                u16 src = 0;
                u32 stamp = 0;
                const RxRing::Pull r = ring.Dequeue(0xFFFF, out, sizeof(out), &got_n, &src, &stamp);
                CHECK_EQ(r == RxRing::Pull::Frame, had);
                if (had && r == RxRing::Pull::Frame) {
                    CHECK_EQ(got_n, n);
                    CHECK_EQ(src, static_cast<u16>(tag));
                    CHECK_EQ(stamp, static_cast<u32>(tag) + 1); // (the stamp given with the frame, through the drops and the wrap)
                    CHECK(std::memcmp(out, Pattern(tag, n).data(), n) == 0); // the bytes survive the wrap
                }
            }
            // never more than total-1 units, and exactly what the model stores
            CHECK(ring.used_units() <= ring.total_units() - 1);
            CHECK_EQ(ring.used_units(), model.Stored());
            CHECK_EQ(ring.frames(), static_cast<u32>(model.entries.size()));
            CHECK_EQ(ring.enqueued(), model.enq);
            CHECK_EQ(ring.overwritten(), model.over);
        }
        CHECK(refused > 0 && nothing > 0); // the random run reached both kinds of refusal
        std::printf("  ring %5u B (%u units): enqueued %u, overwritten %u\n", bytes, ring.total_units(),
                    ring.enqueued(), ring.overwritten());
    }
}

static void TestRingRules() {
    std::printf("ring: the rules one by one\n");
    RxRing ring;
    CHECK(!ring.Init(0x5F3)); // a ring that cannot hold the largest frame is not a ring
    CHECK(ring.Init(0x5F4));
    CHECK_EQ(ring.total_units(), 47u);
    u8 big[1600] = {};
    u32 n = 0;
    u16 src = 0;
    u8 out[1600];

    // FIFO; source and size come back with the frame
    const u8 a[] = {1, 2, 3}, b[] = {4, 5, 6, 7};
    CHECK(ring.Enqueue(1, 2, 9, a, 3) == RxRing::Put::Stored);
    CHECK(ring.Enqueue(1, 3, 9, b, 4) == RxRing::Put::Stored);
    CHECK_EQ(ring.frames(), 2u);
    CHECK_EQ(ring.used_units(), 2u); // 3 B + 8 = 11 B: one unit each
    CHECK(ring.Dequeue(100, out, sizeof(out), &n, &src) == RxRing::Pull::Frame);
    CHECK_EQ(n, 3u);
    CHECK_EQ(src, 2);
    CHECK(std::memcmp(out, a, 3) == 0);
    CHECK(ring.Dequeue(100, out, sizeof(out), &n, &src) == RxRing::Pull::Frame);
    CHECK_EQ(n, 4u);
    CHECK_EQ(src, 3);
    CHECK(ring.Dequeue(100, out, sizeof(out), &n, &src) == RxRing::Pull::Empty);
    CHECK_EQ(n, 0u);

    // a frame without data is nothing, a frame over 1482 bytes is refused, neither touches the ring
    CHECK(ring.Enqueue(1, 2, 9, nullptr, 0) == RxRing::Put::Nothing);
    CHECK(ring.Enqueue(1, 2, 9, big, 1483) == RxRing::Put::TooBig);
    CHECK_EQ(ring.frames(), 0u);
    CHECK_EQ(ring.enqueued(), 2u);
    CHECK(ring.Enqueue(1, 2, 9, big, 1482) == RxRing::Put::Stored); // 47 units: the whole ring ...
    CHECK_EQ(ring.frames(), 0u);                                     // ... so it is the one that goes
    CHECK_EQ(ring.overwritten(), 1u);
    CHECK_EQ(ring.enqueued(), 3u);

    // the largest frame that survives in a 0x5F4 ring is 1464 bytes (46 units = total-1); 1465 does not
    CHECK(ring.Enqueue(1, 2, 9, big, 1464) == RxRing::Put::Stored);
    CHECK_EQ(ring.frames(), 1u);
    CHECK_EQ(ring.used_units(), 46u);
    CHECK(ring.Enqueue(1, 2, 9, big, 1478) == RxRing::Put::Stored); // pushes the 1464 out, then itself
    CHECK_EQ(ring.frames(), 0u);
    CHECK_EQ(ring.overwritten(), 3u);
    CHECK(ring.Enqueue(1, 2, 9, big, 1465) == RxRing::Put::Stored);
    CHECK_EQ(ring.frames(), 0u);

    // a frame that is too large for the caller's buffer stays queued, and says whose it is
    const std::vector<u8> p = Pattern(5, 200);
    CHECK(ring.Enqueue(1, 6, 9, p.data(), 200) == RxRing::Put::Stored);
    CHECK(ring.Dequeue(199, out, sizeof(out), &n, &src) == RxRing::Pull::TooLarge);
    CHECK_EQ(src, 6);
    CHECK_EQ(n, 0u);
    CHECK_EQ(ring.frames(), 1u);
    CHECK(ring.Dequeue(200, out, 150, &n, &src) == RxRing::Pull::Frame); // a short destination: n is the frame's
    CHECK_EQ(n, 200u);
    CHECK(std::memcmp(out, p.data(), 150) == 0);
    CHECK_EQ(ring.frames(), 0u);

    // pulling without a destination just takes the frame out
    CHECK(ring.Enqueue(1, 6, 9, p.data(), 200) == RxRing::Put::Stored);
    CHECK(ring.Dequeue(200, nullptr, 0, &n, &src) == RxRing::Pull::Frame);
    CHECK_EQ(n, 200u);

    // wrap: a frame of 1000 bytes takes 32 of the 47 units, so every other one wraps around the end
    for (int round = 0; round < 40; ++round) {
        const std::vector<u8> w = Pattern(round, 1000);
        CHECK(ring.Enqueue(1, static_cast<u16>(round), 9, w.data(), 1000) == RxRing::Put::Stored);
        CHECK(ring.Dequeue(1000, out, sizeof(out), &n, &src) == RxRing::Pull::Frame);
        CHECK_EQ(n, 1000u);
        CHECK(std::memcmp(out, w.data(), 1000) == 0);
    }

    // Flush: counts and empties; the counters of the bind go on
    const u32 enq = ring.enqueued();
    for (int i = 0; i < 5; ++i) {
        CHECK(ring.Enqueue(1, 2, 9, a, 3) == RxRing::Put::Stored);
    }
    CHECK_EQ(ring.Flush(), 5u);
    CHECK_EQ(ring.frames(), 0u);
    CHECK_EQ(ring.used_units(), 0u);
    CHECK_EQ(ring.enqueued(), enq + 5);
    CHECK_EQ(ring.Flush(), 0u);

    // a ring is as large as asked, in whole units: FFE's 0x2E30 is 369 of them
    RxRing r2;
    CHECK(r2.Init(0x2E30));
    CHECK_EQ(r2.total_units(), 369u);
    CHECK(r2.Init(11839));
    CHECK_EQ(r2.total_units(), 369u);
    CHECK(r2.Init(11840));
    CHECK_EQ(r2.total_units(), 370u);
}

// ---------------------------------------------------------------------------------------------
// 2. The bind table of the engine
// ---------------------------------------------------------------------------------------------

// Counts how often every event was closed: a bind that goes away closes its event exactly once.
class CloseCountingKernel : public FakeKernel {
public:
    void CloseEvent(u32 h) override {
        {
            std::lock_guard<std::mutex> l(cm_);
            ++closes_[h];
        }
        FakeKernel::CloseEvent(h);
    }
    int Closes(u32 h) {
        std::lock_guard<std::mutex> l(cm_);
        return closes_.count(h) ? closes_[h] : 0;
    }

private:
    std::mutex cm_;
    std::map<u32, int> closes_;
};

// A host (node 1) and clients (nodes 2, 3) of our own engine over the mock room.
struct Lobby {
    Lobby() : host(room, 1, &host_kernel), a(room, 2, &a_kernel), b(room, 3, &b_kernel) {
        u32 ev;
        CHECK_EQ(host.engine->Initialize(MakeNode(1, "Host", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        CHECK_EQ(a.engine->Initialize(MakeNode(2, "A", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        CHECK_EQ(b.engine->Initialize(MakeNode(3, "B", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        Host();
    }
    void Host() {
        NetworkInfo ni = MakeNetworkInfo(4, 0);
        CHECK_EQ(host.engine->BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni)), 0u);
    }
    void Join(Console& c) {
        NetworkInfo ni = MakeNetworkInfo(4, 0);
        ni.host_mac_address = host.mac;
        ni.channel = 11;
        CHECK_EQ(c.engine->ConnectToNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni), 1), 0u);
        room.WaitIdle();
    }
    /// A frame from `from` to node `to` (unicast MAC), `n` bytes long; its first two bytes say which one.
    void SendNoWait(Console& from, u16 to, u8 channel, u16 seq, u32 n = 20) {
        std::vector<u8> data = Pattern(seq, n);
        data[0] = static_cast<u8>(seq);
        data[1] = static_cast<u8>(seq >> 8);
        CHECK_EQ(from.engine->SendTo(to, channel, n, 0, data.data(), n), 0u);
    }
    void Send(Console& from, u16 to, u8 channel, u16 seq, u32 n = 20) {
        SendNoWait(from, to, channel, seq, n);
        room.WaitIdle();
    }
    MockRoom room;
    CloseCountingKernel host_kernel, a_kernel, b_kernel;
    Console host, a, b;
};

static void ResetScenario() {
    g_rules.Reset();
    std::memset(&g_stats, 0, sizeof(g_stats));
}

static u32 Pull(UdsEngine& e, u32 bind, std::vector<u8>* out, u32* size, u16* src) {
    return PullInto(e, bind, out, size, src);
}

/// Pulls everything the bind has queued; returns the sequence numbers (see Lobby::Send) in the order they came.
static std::vector<u16> DrainSeqs(UdsEngine& e, u32 bind) {
    std::vector<u16> seqs;
    for (int i = 0; i < 100000; ++i) {
        std::vector<u8> out;
        u32 size = 0;
        u16 src = 0;
        CHECK_EQ(Pull(e, bind, &out, &size, &src), 0u);
        if (size == 0) {
            break;
        }
        seqs.push_back(static_cast<u16>(out[0] | out[1] << 8));
    }
    return seqs;
}

static void TestBindTable() {
    std::printf("engine: bind table (lookup, 16 entries, events, counters)\n");
    ResetScenario();
    Lobby lobby;
    lobby.Join(lobby.a);
    lobby.Join(lobby.b);
    UdsEngine& a = *lobby.a.engine;
    u32 ev1, ev2, ev3, ev4, ev;

    // Several binds on one channel are fine; a frame goes to the first bind of its sender, else to the LAST
    // bind that takes everybody (nwm's lookup: an exact source returns at once, wildcards overwrite each other)
    CHECK_EQ(a.Bind(10, 0x5F4, 5, kBroadcastNetworkNodeId, &ev1), 0u);
    CHECK_EQ(a.Bind(11, 0x5F4, 5, kBroadcastNetworkNodeId, &ev2), 0u);
    CHECK_EQ(a.Bind(12, 0x5F4, 5, 1, &ev3), 0u); // only the host
    CHECK_EQ(a.Bind(13, 0x5F4, 6, 3, &ev4), 0u); // channel 6: only node 3
    CHECK(ev1 != ev2 && ev2 != ev3 && ev3 != ev4);
    lobby.Send(lobby.host, 2, 5, 100); // from node 1: the exact bind (12)
    lobby.Send(lobby.b, 2, 5, 101);    // from node 3: no exact bind, the last wildcard (11)
    lobby.Send(lobby.b, 2, 6, 102);    // channel 6 from node 3: bind 13
    lobby.Send(lobby.host, 2, 6, 103); // channel 6 from the host: nobody's
    lobby.Send(lobby.host, 2, 7, 104); // channel 7: nobody bound it
    CHECK(DrainSeqs(a, 10) == std::vector<u16>{});
    CHECK(DrainSeqs(a, 11) == std::vector<u16>{101});
    CHECK(DrainSeqs(a, 12) == std::vector<u16>{100});
    CHECK(DrainSeqs(a, 13) == std::vector<u16>{102});
    CHECK_EQ(g_stats.drop_src_filter, 1u);
    CHECK_EQ(g_stats.drop_no_bind, 1u);

    // one event per stored frame
    CHECK_EQ(lobby.a_kernel.Signals(ev2), 1u);
    CHECK_EQ(lobby.a_kernel.Signals(ev3), 1u);
    CHECK_EQ(lobby.a_kernel.Signals(ev1), 0u);

    // Unbind: the id comes back with the channel, the source, what was stored and what was pushed out; the
    // event is signalled once and closed; the id is unknown afterwards
    UdsEngine::UnbindInfo info;
    CHECK_EQ(a.Unbind(12, &info), 0u);
    CHECK_EQ(info.channel, 5);
    CHECK_EQ(info.src, 1);
    CHECK_EQ(info.enqueued, 1u);
    CHECK_EQ(info.overwritten, 0u);
    CHECK_EQ(lobby.a_kernel.Signals(ev3), 2u);
    CHECK_EQ(lobby.a_kernel.Closes(ev3), 1);
    std::vector<u8> out;
    u32 size = 0;
    u16 src = 0;
    CHECK_EQ(Pull(a, 12, &out, &size, &src), res::kBadId);
    CHECK_EQ(a.Unbind(12, &info), 0u); // not there: success, nothing to report
    CHECK_EQ(info.enqueued, 0u);
    CHECK_EQ(a.Unbind(0, &info), res::kBadId);
    lobby.Send(lobby.host, 2, 5, 105); // bind 12 is gone: the host's frame goes to the wildcard
    CHECK(DrainSeqs(a, 11) == std::vector<u16>{105});

    // ids are not checked for duplicates: two binds of one id are two binds, Unbind takes the first and the
    // second answers from then on
    CHECK_EQ(a.Bind(30, 0x5F4, 40, 3, &ev), 0u);
    CHECK_EQ(a.Bind(30, 0x5F4, 41, 3, &ev), 0u);
    CHECK_EQ(a.Unbind(30, &info), 0u);
    CHECK_EQ(info.channel, 40);
    CHECK_EQ(a.Unbind(30, &info), 0u);
    CHECK_EQ(info.channel, 41);
    CHECK_EQ(a.Unbind(30, &info), 0u);
    CHECK_EQ(info.channel, 0);

    // 16 binds at most (a 17th is out of resource), a freed entry is used again
    CHECK_EQ(a.Unbind(10, nullptr), 0u);
    CHECK_EQ(a.Unbind(11, nullptr), 0u);
    CHECK_EQ(a.Unbind(13, nullptr), 0u);
    for (u32 id = 1; id <= 16; ++id) {
        CHECK_EQ(a.Bind(id, 0x5F4, static_cast<u8>(20 + id), kBroadcastNetworkNodeId, &ev), 0u);
    }
    CHECK_EQ(a.Bind(17, 0x5F4, 40, kBroadcastNetworkNodeId, &ev), res::kOutOfMemory);
    CHECK_EQ(a.Unbind(7, nullptr), 0u);
    CHECK_EQ(a.Bind(17, 0x5F4, 40, kBroadcastNetworkNodeId, &ev), 0u);
    CHECK_EQ(a.Unbind(17, nullptr), 0u);

    // the arguments: id 0 and channel 0 are bad ids, a ring under 0x5F4 is out of resource
    CHECK_EQ(a.Bind(0, 0x5F4, 1, 0xFFFF, &ev), res::kBadId);
    CHECK_EQ(a.Bind(99, 0x5F4, 0, 0xFFFF, &ev), res::kBadId);
    CHECK_EQ(a.Bind(99, 0x5F3, 1, 0xFFFF, &ev), res::kOutOfMemory);
    for (u32 id = 1; id <= 16; ++id) {
        a.Unbind(id, nullptr);
    }
    CHECK_RULES();
}

// nwm queues a frame only if it carries data and fits an aggregate (1482 bytes at most).
static void TestOddFrames() {
    std::printf("engine: frames without data or over 1482 bytes are not queued\n");
    ResetScenario();
    Lobby lobby;
    lobby.Join(lobby.a);
    UdsEngine& a = *lobby.a.engine;
    u32 ev;
    CHECK_EQ(a.Bind(2, 0x5F4, 5, kBroadcastNetworkNodeId, &ev), 0u);
    auto frame = [&](u32 n) {
        WifiPacket p;
        p.type = WifiPacket::Type::Data;
        p.channel = 11;
        p.transmitter_address = lobby.host.mac;
        p.destination_address = lobby.a.mac;
        const std::vector<u8> payload(n, 0x11);
        p.data = GenerateDataPayload(payload.data(), n, 5, 2, 1, 0);
        a.OnWifiPacket(p);
    };
    frame(0);
    frame(1483);
    CHECK_EQ(g_stats.drop_bad_size, 2u);
    CHECK_EQ(g_stats.data_delivered, 0u);
    CHECK_EQ(lobby.a_kernel.Signals(ev), 0u);
    frame(1482);
    frame(1);
    CHECK_EQ(g_stats.data_delivered, 2u);
    CHECK_EQ(lobby.a_kernel.Signals(ev), 2u);
    std::vector<u8> out;
    u32 size = 0;
    u16 src = 0;
    CHECK_EQ(Pull(a, 2, &out, &size, &src), res::kTooLarge); // (the SDK's limit is 0x5C6: the frame stays)
    CHECK_EQ(a.PullPacket(2, 0x5D4, out.data(), static_cast<u32>(out.size()), &size, &src), 0u);
    CHECK_EQ(size, 1482u); // (more than the 0x5C8 bytes the SDK's buffer holds: the copy stops there, the size does not)
    CHECK_EQ(Pull(a, 2, &out, &size, &src), 0u);
    CHECK_EQ(size, 1u);
    CHECK_EQ(Pull(a, 2, &out, &size, &src), 0u);
    CHECK_EQ(size, 0u);
    CHECK_RULES();
}

static void TestBindAdmission() {
    std::printf("engine: the binds fit the block the game gave Initialize\n");
    ResetScenario();
    MockRoom room;
    Console c(room, 5);
    u32 ev;
    // 0x14000 as FFE declares it: the heap of nwm over it takes 16 bytes for itself and, per bind, 16 bytes of
    // header and the size rounded up to 4
    CHECK_EQ(c.engine->Initialize(MakeNode(5, "C", 0), 0x14000, kTestVersion, &ev), 0u);
    UdsEngine& e = *c.engine;
    const u32 room_left = 0x14000 - 16;
    CHECK_EQ(e.Bind(1, room_left - 16 + 1, 1, 0xFFFF, &ev), res::kOutOfMemory); // one byte too many
    CHECK_EQ(e.Bind(1, room_left - 16, 1, 0xFFFF, &ev), 0u);                   // exactly the room there is
    CHECK_EQ(e.Bind(2, 0x5F4, 2, 0xFFFF, &ev), res::kOutOfMemory);             // nothing left
    CHECK_EQ(e.Unbind(1, nullptr), 0u);
    for (u32 id = 2; id <= 7; ++id) { // FFE's own bind, six times
        CHECK_EQ(e.Bind(id, 0x2E30, static_cast<u8>(id), 0xFFFF, &ev), 0u);
    }
    CHECK_EQ(e.Bind(8, 0x2E30, 8, 0xFFFF, &ev), res::kOutOfMemory); // not a seventh
    // 6 x (16 + 11,824) = 71,040 of 81,904: 10,864 are left, i.e. a size of 10,848
    CHECK_EQ(e.Bind(8, 10849, 8, 0xFFFF, &ev), res::kOutOfMemory); // (10,849 counts as 10,852)
    CHECK_EQ(e.Bind(8, 10848, 8, 0xFFFF, &ev), 0u);
    CHECK_EQ(e.Unbind(8, nullptr), 0u);
    CHECK_EQ(e.Unbind(4, nullptr), 0u); // a bind that goes gives its room back
    CHECK_EQ(e.Bind(8, 0x2E30, 8, 0xFFFF, &ev), 0u);
    CHECK_EQ(e.Shutdown(), 0u); // the Shutdown frees all of it
    CHECK_EQ(e.Initialize(MakeNode(5, "C", 0), 0x14000, kTestVersion, &ev), 0u);
    CHECK_EQ(e.Bind(1, room_left - 16, 1, 0xFFFF, &ev), 0u);
    CHECK_EQ(e.Shutdown(), 0u);

    // a game that gave a block of 0 (nothing to check against): any size goes
    CHECK_EQ(e.Initialize(MakeNode(5, "C", 0), 0, kTestVersion, &ev), 0u);
    CHECK_EQ(e.Bind(1, 0x20000, 1, 0xFFFF, &ev), 0u);
    CHECK_EQ(e.Shutdown(), 0u);
    CHECK_RULES();
}

static void TestRingFloor() {
    std::printf("engine: a ring holds at least 16 KiB however small the game's bind is (spec C2)\n");
    ResetScenario();
    Lobby lobby;
    lobby.Join(lobby.a);
    UdsEngine& a = *lobby.a.engine;
    u32 ev;
    CHECK_EQ(a.Bind(2, 0x5F4, 5, kBroadcastNetworkNodeId, &ev), 0u); // the smallest bind: 47 units declared
    // 140-byte frames take 5 units; a ring of 16 KiB (512 units) holds 511 -> 102 of them
    for (u16 seq = 0; seq < 110; ++seq) {
        lobby.SendNoWait(lobby.host, 2, 5, seq, 140);
    }
    lobby.room.WaitIdle();
    const std::vector<u16> got = DrainSeqs(a, 2);
    CHECK_EQ(got.size(), std::size_t(102));
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK_EQ(got[i], 8 + i); // the 8 oldest were pushed out, the rest is in order
    }
    UdsEngine::UnbindInfo info;
    CHECK_EQ(a.Unbind(2, &info), 0u);
    CHECK_EQ(info.enqueued, 110u);
    CHECK_EQ(info.overwritten, 8u);
    CHECK_EQ(g_stats.ring_peak_units, 510u); // the fullest a ring got: 102 frames of 5 units

    // FFE's own 0x2E30 (369 units) is lifted to the same floor
    CHECK_EQ(a.Bind(3, 0x2E30, 5, kBroadcastNetworkNodeId, &ev), 0u);
    for (u16 seq = 0; seq < 110; ++seq) {
        lobby.SendNoWait(lobby.host, 2, 5, seq, 140);
    }
    lobby.room.WaitIdle();
    CHECK_EQ(DrainSeqs(a, 3).size(), std::size_t(102));
    CHECK_EQ(a.Unbind(3, nullptr), 0u);

    // a game that asks for more gets it: 0x13FE0 bytes (the whole block FFE declares) hold 2559 units
    CHECK_EQ(a.Bind(4, 0x14000 - 32, 5, kBroadcastNetworkNodeId, &ev), 0u);
    for (u16 seq = 0; seq < 400; ++seq) {
        lobby.SendNoWait(lobby.host, 2, 5, seq, 140);
    }
    lobby.room.WaitIdle();
    CHECK_EQ(DrainSeqs(a, 4).size(), std::size_t(400));
    CHECK_EQ(g_stats.ring_overwritten, 16u); // (the first two rounds, 8 each)
    CHECK_EQ(g_stats.ring_peak_units, 2000u); // 400 frames of 5 units in the big ring
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------
// 3. Binds survive the end of a session, the frames do not
// ---------------------------------------------------------------------------------------------
static void TestBindsSurvive() {
    std::printf("engine: binds survive Disconnect / Destroy, queued frames are flushed at the next session\n");
    ResetScenario();
    Lobby lobby;
    UdsEngine& a = *lobby.a.engine;
    UdsEngine& host = *lobby.host.engine;
    u32 ev, hev;
    CHECK_EQ(a.Bind(2, 0x2E30, 1, kBroadcastNetworkNodeId, &ev), 0u);
    CHECK_EQ(host.Bind(2, 0x2E30, 1, kBroadcastNetworkNodeId, &hev), 0u);
    lobby.Join(lobby.a);
    lobby.Send(lobby.host, 2, 1, 1);
    lobby.Send(lobby.host, 2, 1, 2);
    CHECK_EQ(lobby.a_kernel.Signals(ev), 2u);

    // Disconnect: the bind event is woken once (the real service's tail), the bind stays and so do the frames
    // - which cannot be pulled in the state the game is in (wrong state)
    CHECK_EQ(a.DisconnectNetwork(), 0u);
    CHECK_EQ(lobby.a_kernel.Signals(ev), 3u);
    CHECK_EQ(lobby.a_kernel.Closes(ev), 0);
    std::vector<u8> out;
    u32 size = 0;
    u16 src = 0;
    CHECK_EQ(Pull(a, 2, &out, &size, &src), res::kWrongState);
    CHECK_EQ(g_stats.binds_signalled, 1u);
    lobby.room.WaitIdle();

    // the next Connect: the same bind works without a new Bind, and the two old frames are not delivered
    const u32 flushed = g_stats.ring_flushed;
    lobby.Join(lobby.a);
    CHECK_EQ(g_stats.ring_flushed - flushed, 2u);
    CHECK_EQ(Pull(a, 2, &out, &size, &src), 0u);
    CHECK_EQ(size, 0u); // empty
    lobby.Send(lobby.host, 2, 1, 3);
    CHECK(DrainSeqs(a, 2) == std::vector<u16>{3});
    UdsEngine::UnbindInfo info;
    CHECK_EQ(a.Unbind(2, &info), 0u);
    CHECK_EQ(info.enqueued, 3u); // the counters of the bind run through both sessions
    CHECK_EQ(lobby.a_kernel.Closes(ev), 1);

    // The host: DestroyNetwork keeps its bind; hosting again flushes what was queued in between
    lobby.Join(lobby.b);
    lobby.Send(lobby.b, 1, 1, 10);
    lobby.Send(lobby.b, 1, 1, 11);
    lobby.Send(lobby.b, 1, 1, 12);
    CHECK_EQ(host.DestroyNetwork(), 0u);
    CHECK_EQ(lobby.host_kernel.Closes(hev), 0);
    CHECK_EQ(Pull(host, 2, &out, &size, &src), res::kWrongState);
    lobby.room.WaitIdle();
    const u32 flushed_host = g_stats.ring_flushed;
    lobby.Host();
    CHECK_EQ(g_stats.ring_flushed - flushed_host, 3u);
    CHECK_EQ(Pull(host, 2, &out, &size, &src), 0u);
    CHECK_EQ(size, 0u);
    lobby.b.engine->DisconnectNetwork();
    lobby.room.WaitIdle();
    CHECK_EQ(host.DestroyNetwork(), 0u);

    // Shutdown frees the binds: the event is signalled once more and closed, exactly once
    const u32 signals = lobby.host_kernel.Signals(hev);
    CHECK_EQ(host.Shutdown(), 0u);
    CHECK_EQ(lobby.host_kernel.Signals(hev), signals + 1);
    CHECK_EQ(lobby.host_kernel.Closes(hev), 1);
    CHECK_EQ(host.Shutdown(), 0u); // (a second one finds nothing to free)
    CHECK_EQ(lobby.host_kernel.Closes(hev), 1);
    u32 ev2;
    CHECK_EQ(host.Initialize(MakeNode(1, "Host", 0), kTestSharedMemory, kTestVersion, &ev2), 0u);
    CHECK_EQ(host.Unbind(2, &info), 0u);
    CHECK_EQ(info.enqueued, 0u); // the bind went with the session
    CHECK_RULES();
}

// FFE's exit: Unbind(2) -> Bind(4) -> Disconnect -> Shutdown. The bind that is left is woken by the Disconnect
// and freed by the Shutdown: signalled and closed once each, no handle left over.
static void TestFfeExitOrder() {
    std::printf("engine: FFE's exit order frees the last bind exactly once\n");
    ResetScenario();
    Lobby lobby;
    UdsEngine& a = *lobby.a.engine;
    u32 ev2, ev4;
    CHECK_EQ(a.Bind(2, 0x2E30, 0xF3, 0xFFFF, &ev2), 0u);
    lobby.Join(lobby.a);
    CHECK_EQ(a.Unbind(2, nullptr), 0u);
    CHECK_EQ(a.Bind(4, 0x2E30, 0xF3, 0xFFFF, &ev4), 0u);
    CHECK_EQ(a.DisconnectNetwork(), 0u);
    CHECK_EQ(lobby.a_kernel.Signals(ev4), 1u);
    CHECK_EQ(a.Shutdown(), 0u);
    CHECK_EQ(lobby.a_kernel.Signals(ev4), 2u);
    CHECK_EQ(lobby.a_kernel.Closes(ev4), 1);
    CHECK_EQ(lobby.a_kernel.Closes(ev2), 1);
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------
// 4. The exit race
// ---------------------------------------------------------------------------------------------
static void TestExitRace() {
    std::printf("engine: Unbind and PullPacket of the same bind on two threads (FFE does this at exit)\n");
    ResetScenario();
    Lobby lobby;
    lobby.Join(lobby.a);
    UdsEngine& a = *lobby.a.engine;
    u32 ev;
    int ok_pulls = 0, bad_pulls = 0;
    u32 odd_result = 0;
    for (int round = 0; round < 40; ++round) {
        CHECK_EQ(a.Bind(2, 0x2E30, 1, kBroadcastNetworkNodeId, &ev), 0u);
        CHECK_EQ(a.Bind(4, 0x2E30, 2, kBroadcastNetworkNodeId, &ev), 0u);
        for (u16 s = 0; s < 6; ++s) {
            lobby.SendNoWait(lobby.host, 2, 1, s);
        }
        lobby.Send(lobby.host, 2, 2, 77);
        std::atomic<bool> go{false};
        std::atomic<int> pulled{0}; // successful pulls so far
        std::atomic<int> state{0};  // 1 = the puller saw the bind gone
        std::thread puller([&] {
            while (!go.load()) {
                std::this_thread::yield();
            }
            for (int i = 0; i < 200000 && state.load() == 0; ++i) {
                u32 size = 0;
                u16 src = 0;
                const u32 r = a.PullPacket(2, 0x5C6, nullptr, 0, &size, &src);
                if (r == 0) {
                    ++ok_pulls;
                    pulled.fetch_add(1);
                } else {
                    ++bad_pulls;
                    odd_result = r == res::kBadId ? odd_result : r; // anything but "no such bind" would be a bug
                    state = 1;
                }
            }
        });
        go = true;
        // The Unbind comes while the puller is pulling, not before it ran: on a machine with every core busy the
        // thread may not have started after a few hundred microseconds, and there would be no race to test.
        for (int i = 0; i < 5000 && pulled.load() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50 * (round % 7)));
        CHECK_EQ(a.Unbind(2, nullptr), 0u);
        puller.join();
        CHECK_EQ(state.load(), 1);
        // the other bind is untouched, its frame still there
        std::vector<u8> out;
        u32 size = 0;
        u16 src = 0;
        CHECK_EQ(Pull(a, 4, &out, &size, &src), 0u);
        CHECK_EQ(size, 20u);
        CHECK_EQ(a.Unbind(4, nullptr), 0u);
        lobby.room.WaitIdle();
    }
    CHECK(ok_pulls > 0 && bad_pulls == 40);
    CHECK_EQ(odd_result, 0u);
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------
// 5. The load model (spec C2)
// ---------------------------------------------------------------------------------------------

// The frames of the busiest 6 s of the two FFE captures reach a ring at their own pace (or `rate` times
// faster) while the game does not pull for `stall_ms` from the time of frame `first_stalled`; outside the
// stall it pulls every frame at once. Returns how many frames the ring had to push out. Times are kept in
// the captures' milliseconds (a window of S ms at `rate` times the speed is S * rate of them).
static u32 Replay(const LoadFrame* frames, std::size_t count, bool drop_copies, u32 rate, u32 ring_bytes,
                  u32 stall_ms, std::size_t first_stalled) {
    RxRing ring;
    ring.Init(ring_bytes);
    std::vector<u8> payload(1500, 0x5A);
    long long t = 0, stall_from = -1, stall_to = -1;
    u8 out[1600];
    u32 n = 0;
    u16 src = 0;
    auto catch_up = [&] {
        while (ring.Dequeue(0xFFFF, out, sizeof(out), &n, &src) == RxRing::Pull::Frame) {
        }
    };
    for (std::size_t i = 0; i < count; ++i) {
        t += frames[i].dt_ms;
        if (i == first_stalled) {
            stall_from = t;
            stall_to = t + static_cast<long long>(stall_ms) * rate;
        }
        if (drop_copies && frames[i].copy) {
            continue; // the twin filter (F13) never lets the second copy reach the ring
        }
        const bool stalled = t >= stall_from && t < stall_to;
        if (!stalled) {
            catch_up(); // (the game is back: it reads what waited before the next frame comes in)
        }
        ring.Enqueue(1, 2, 9, payload.data(), frames[i].size);
        if (!stalled) {
            catch_up();
        }
    }
    return ring.overwritten();
}

/// The most ring units that arrive within `window_ms` of a game that runs `rate` times faster: what is in
/// the ring when the game has been away for that long, at worst.
static u32 NeedUnits(const LoadFrame* frames, std::size_t count, bool drop_copies, u32 rate, u32 window_ms) {
    std::vector<std::pair<long long, u32>> pts;
    long long t = 0;
    for (std::size_t i = 0; i < count; ++i) {
        t += frames[i].dt_ms;
        if (!(drop_copies && frames[i].copy)) {
            pts.push_back({t, RxRing::UnitsFor(frames[i].size)});
        }
    }
    const long long window = static_cast<long long>(window_ms) * rate;
    u32 best = 0, sum = 0;
    std::size_t from = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        sum += pts[i].second;
        while (pts[from].first <= pts[i].first - window) {
            sum -= pts[from].second;
            ++from;
        }
        best = std::max(best, sum);
    }
    return best;
}

static u32 WorstLoss(const LoadFrame* frames, std::size_t count, bool drop_copies, u32 rate, u32 ring_bytes,
                     u32 stall_ms) {
    u32 worst = 0;
    for (std::size_t first = 0; first < count; ++first) {
        worst = std::max(worst, Replay(frames, count, drop_copies, rate, ring_bytes, stall_ms, first));
    }
    return worst;
}

static void TestLoadModel() {
    std::printf("ring: the load of two real FFE captures with a game that stops pulling for a while\n");
    struct Capture {
        const char* name;
        const LoadFrame* frames;
        std::size_t count;
    } captures[] = {{"A", kLoadA, sizeof(kLoadA) / sizeof(kLoadA[0])}, {"B", kLoadB, sizeof(kLoadB) / sizeof(kLoadB[0])}};
    const u32 stalls[] = {250, 400, 1000, 2100};
    const u32 kDeclared = 0x2E30; // what FFE binds: 369 units, 368 of them usable
    const u32 kFloor = 0x4000;    // what the engine gives every bind at least: 512 units, 511 usable

    for (const Capture& c : captures) {
        u64 units = 0;
        u32 largest = 0;
        for (std::size_t i = 0; i < c.count; ++i) {
            units += RxRing::UnitsFor(c.frames[i].size);
            largest = std::max(largest, RxRing::UnitsFor(c.frames[i].size));
        }
        CHECK(largest <= 41); // (FFE's biggest frame is 1285 bytes)
        std::printf("  capture %s: %zu frames, %.1f units each, the busiest second takes %u units\n", c.name, c.count,
                    static_cast<double>(units) / c.count, NeedUnits(c.frames, c.count, false, 1, 1000));

        // The ring loses frames exactly when more units arrive than it holds while the game is away.
        for (u32 rate : {1u, 3u}) {
            for (bool dedupe : {false, true}) {
                for (u32 bytes : {kDeclared, kFloor}) {
                    for (u32 stall : stalls) {
                        const u32 need = NeedUnits(c.frames, c.count, dedupe, rate, stall);
                        const u32 usable = bytes / 32 - 1;
                        const u32 lost = WorstLoss(c.frames, c.count, dedupe, rate, bytes, stall);
                        if (need <= usable) {
                            CHECK_EQ(lost, 0u);
                        } else {
                            CHECK(lost > 0);
                        }
                    }
                }
            }
        }

        // What the floor is meant to cover: a stall of 0.4 s at the rate of the captures once the twin filter
        // has removed the host's copies (0.25 s without it).
        CHECK_EQ(WorstLoss(c.frames, c.count, true, 1, kFloor, 250), 0u);
        CHECK_EQ(WorstLoss(c.frames, c.count, true, 1, kFloor, 400), 0u);
        CHECK_EQ(WorstLoss(c.frames, c.count, false, 1, kFloor, 250), 0u);
    }
    // the busier capture: 0.4 s do not fit the ring FFE declares, they fit the floor
    CHECK(WorstLoss(kLoadB, sizeof(kLoadB) / sizeof(kLoadB[0]), true, 1, kDeclared, 400) > 0);
    CHECK_EQ(WorstLoss(kLoadB, sizeof(kLoadB) / sizeof(kLoadB[0]), true, 1, kFloor, 400), 0u);

    // beyond what a ring holds the oldest frames go and the newest stay: it is never blocked and never grows
    RxRing ring;
    CHECK(ring.Init(kFloor));
    std::vector<u8> payload(140, 1);
    for (u16 i = 0; i < 5000; ++i) {
        ring.Enqueue(1, i, 9, payload.data(), 140);
    }
    u8 out[200];
    u32 n = 0;
    u16 src = 0;
    CHECK(ring.Dequeue(1000, out, sizeof(out), &n, &src) == RxRing::Pull::Frame);
    CHECK_EQ(src, 5000 - 102); // the oldest one still there: 102 frames of 5 units fit
}

int main() {
    TestRingAgainstModel();
    TestRingRules();
    TestBindTable();
    TestOddFrames();
    TestBindAdmission();
    TestRingFloor();
    TestBindsSurvive();
    TestFfeExitOrder();
    TestExitRace();
    TestLoadModel();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
