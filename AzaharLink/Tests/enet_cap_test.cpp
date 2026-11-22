// The limit of waiting data of the ENet client (ENetHost::maximumWaitingData, 192 KB in the plugin) and its dead end (2026-11-14, Mario Party:
// Island Tour hosted by a console: five sessions of five ended in it). ENet refuses every command that would be queued while the data that waits
// is at the limit; when all of it waits behind a hole of the reliable sequence, the command that closes the hole was refused with the rest and the
// room's stream never moved again. What is held here: that state made by hand and the way out of it, what is refused and what is acknowledged, the
// most the waiting data can ever be, a datagram that is read on after a refused command, a large packet as the one that closes the hole, the two
// counters and the peak, and the lag of the stream (ENetAzlCounters::streamLagMaxMs).
//
// The client is a real ENet host connected to a real one on the loopback; the "room" of these scenes is the test itself: it writes the datagrams
// byte by byte (a header, SEND_RELIABLE / SEND_FRAGMENT commands with the sequence numbers it wants) and sends them from the server's socket, so a
// scene has exactly the holes it asks for. The link wraps enet_socket_send (the acknowledgements the client sends are read back from its datagrams)
// and enet_time_get (ENet's clock moves only when the test moves it). What a real room does when it is not acknowledged is the model's part
// (Tests/vsim, ENet 1.3.18 unmodified on the room's side) and the real room's (Tests/integration.sh).
//
// The same source builds against a tree WITHOUT the remedy (no ENET_AZL_LAG_LEAK_MS in its enet.h): there it shows the dead end - the scene
// "the way out" fails - which is how the state was reproduced before it was corrected.
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <enet/enet.h>

#include "support.hpp"

using namespace azl;

namespace {

constexpr std::size_t kLimit = 192 * 1024; // the plugin's (Sources/azl/room_client.cpp, kMaxWaitingData) ...
constexpr std::size_t kPacketMax = 64 * 1024; // ... and kMaxPacketSize

enet_uint32 g_clock = 1000000;
int g_client_socket = -1;
std::set<u32> g_acked; ///< the reliable sequence numbers (channel 0) the client has acknowledged, read from the datagrams it sent
u32 g_client_datagrams = 0; ///< datagrams the client handed to its socket ...
u32 g_client_largest = 0;   ///< ... and the largest of them (bytes)

} // namespace

extern "C" {
int __real_enet_socket_send(ENetSocket, const ENetAddress*, const ENetBuffer*, size_t);
enet_uint32 __wrap_enet_time_get(void) {
    return g_clock;
}
int __wrap_enet_socket_send(ENetSocket s, const ENetAddress* a, const ENetBuffer* b, size_t n) {
    if (s == g_client_socket) {
        std::vector<u8> d;
        for (size_t i = 0; i < n; ++i) {
            const u8* p = static_cast<const u8*>(b[i].data);
            d.insert(d.end(), p, p + b[i].dataLength);
        }
        ++g_client_datagrams;
        if (d.size() > g_client_largest) {
            g_client_largest = static_cast<u32>(d.size());
        }
        if (d.size() >= 2) {
            const u16 peer_id = static_cast<u16>((d[0] << 8) | d[1]);
            std::size_t at = (peer_id & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME) ? 4 : 2;
            while (at + 4 <= d.size()) {
                const u8 command = d[at] & ENET_PROTOCOL_COMMAND_MASK;
                const std::size_t size = command < ENET_PROTOCOL_COMMAND_COUNT ? enet_protocol_command_size(command) : 0;
                if (size == 0 || at + size > d.size()) {
                    break;
                }
                if (command == ENET_PROTOCOL_COMMAND_ACKNOWLEDGE && d[at + 1] == 0) {
                    g_acked.insert(static_cast<u32>((d[at + 4] << 8) | d[at + 5]));
                } else if (command != ENET_PROTOCOL_COMMAND_ACKNOWLEDGE && command != ENET_PROTOCOL_COMMAND_PING &&
                           command != ENET_PROTOCOL_COMMAND_THROTTLE_CONFIGURE) {
                    break; // (a command with data after it: the client sends none here)
                }
                at += size;
            }
        }
    }
    return __real_enet_socket_send(s, a, b, n);
}
}

namespace {

/// A client connected to a server on the loopback, with the plugin's limits, and the room written by hand.
struct Rig {
    ENetHost* server = nullptr;
    ENetHost* client = nullptr;
    ENetPeer* cp = nullptr;
    ENetAddress client_address{};
    u16 base = 0;              ///< the last reliable sequence number the client has (channel 0) before the scene
    std::vector<u32> got;      ///< the packets the client was handed: their sequence numbers (the first four bytes of the data)
    std::size_t peak = 0;      ///< the most data that waited, read after every datagram

    Rig() {
        ENetAddress address{};
        address.host = ENET_HOST_ANY;
        address.port = 0;
        server = enet_host_create(&address, 1, 1, 0, 0);
        client = enet_host_create(nullptr, 1, 1, 0, 0);
        CHECK(server != nullptr && client != nullptr);
        ENetAddress to{};
        enet_address_set_host(&to, "127.0.0.1");
        to.port = server->address.port;
        cp = enet_host_connect(client, &to, 1, 0);
        ENetPeer* sp = &server->peers[0];
        for (int i = 0; i < 2000 && !(cp->state == ENET_PEER_STATE_CONNECTED && sp->state == ENET_PEER_STATE_CONNECTED); ++i) {
            ENetEvent ev;
            enet_host_service(server, &ev, 1);
            enet_host_service(client, &ev, 1);
        }
        CHECK(cp->state == ENET_PEER_STATE_CONNECTED);
        client->maximumPacketSize = kPacketMax;
        client->maximumWaitingData = kLimit;
        g_client_socket = client->socket;
        enet_socket_get_address(client->socket, &client_address);
        enet_address_set_host(&client_address, "127.0.0.1");
        Quiet();
        base = cp->channels[0].incomingReliableSequenceNumber;
        g_acked.clear();
        ENetAzlCountersTake();
    }
    ~Rig() {
        g_client_socket = -1;
        enet_peer_reset(cp);
        enet_host_destroy(client);
        enet_host_destroy(server);
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    void ENetAzlCountersTake() {
        ENetAzlCounters c;
        enet_azl_counters_take(client, &c);
    }
    /// Both hosts serviced until nothing moves (the server only reads what the client sends: acknowledgements, pings).
    void Quiet() {
        for (int i = 0; i < 6; ++i) {
            Service();
            ENetEvent ev;
            while (enet_host_service(server, &ev, 1) > 0) {
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(ev.packet);
                }
            }
        }
    }
    /// The client's service calls until one hands nothing over; at most `take` packets are taken (the caller that is slow: the rest stays ready).
    void Service(int take = 1 << 30) {
        for (int calls = 0; calls < 100000 && take > 0; ++calls) {
            ENetEvent ev;
            const int r = enet_host_service(client, &ev, 0);
            if (r <= 0) {
                break;
            }
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                u32 id = 0;
                if (ev.packet->dataLength >= 4) {
                    std::memcpy(&id, ev.packet->data, 4);
                }
                got.push_back(id);
                enet_packet_destroy(ev.packet);
                --take;
            }
        }
    }

    // ---- the room, by hand ----
    static void Put16(std::vector<u8>* d, u32 v) {
        d->push_back(static_cast<u8>(v >> 8));
        d->push_back(static_cast<u8>(v));
    }
    static void Put32(std::vector<u8>* d, u32 v) {
        Put16(d, v >> 16);
        Put16(d, v & 0xFFFF);
    }
    std::vector<u8> Header(u16 sent_time) const {
        std::vector<u8> d;
        Put16(&d, static_cast<u32>(cp->incomingPeerID) | ENET_PROTOCOL_HEADER_FLAG_SENT_TIME |
                      (static_cast<u32>(cp->incomingSessionID) << ENET_PROTOCOL_HEADER_SESSION_SHIFT));
        Put16(&d, sent_time);
        return d;
    }
    /// SEND_RELIABLE, number `base + n`, `size` bytes of data that start with n.
    void Reliable(std::vector<u8>* d, u32 n, u32 size) const {
        d->push_back(ENET_PROTOCOL_COMMAND_SEND_RELIABLE | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE);
        d->push_back(0);
        Put16(d, static_cast<u16>(base + n));
        Put16(d, size);
        const std::size_t at = d->size();
        d->resize(at + size, 0xA5);
        std::memcpy(d->data() + at, &n, 4);
    }
    /// One fragment of a reliable packet of `total` bytes whose first number is `base + first`: fragment `index` of `count`, `size` bytes at `offset`.
    void Fragment(std::vector<u8>* d, u32 first, u32 index, u32 count, u32 total, u32 offset, u32 size) const {
        d->push_back(ENET_PROTOCOL_COMMAND_SEND_FRAGMENT | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE);
        d->push_back(0);
        Put16(d, static_cast<u16>(base + first + index));
        Put16(d, static_cast<u16>(base + first));
        Put16(d, size);
        Put32(d, count);
        Put32(d, index);
        Put32(d, total);
        Put32(d, offset);
        const std::size_t at = d->size();
        d->resize(at + size, 0x5A);
        if (offset == 0) {
            std::memcpy(d->data() + at, &first, 4);
        }
    }
    /// SEND_UNRELIABLE sent after the reliable number `base + rel` (it waits for that one): unreliable number `seq`, `size` bytes of data that start with `id`.
    void Unreliable(std::vector<u8>* d, u32 rel, u32 seq, u32 id, u32 size) const {
        d->push_back(ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE);
        d->push_back(0);
        Put16(d, static_cast<u16>(base + rel));
        Put16(d, seq);
        Put16(d, size);
        const std::size_t at = d->size();
        d->resize(at + size, 0x3C);
        std::memcpy(d->data() + at, &id, 4);
    }
    /// One fragment of an UNRELIABLE packet of `total` bytes sent after the reliable number `base + rel`: unreliable numbers from `seq`, fragment `index` of
    /// `count`, `size` bytes at `offset`; the data start with `id`.
    void UnreliableFragment(std::vector<u8>* d, u32 rel, u32 seq, u32 index, u32 count, u32 total, u32 offset, u32 size, u32 id) const {
        d->push_back(ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT);
        d->push_back(0);
        Put16(d, static_cast<u16>(base + rel));
        Put16(d, static_cast<u16>(seq));
        Put16(d, size);
        Put32(d, count);
        Put32(d, index);
        Put32(d, total);
        Put32(d, offset);
        const std::size_t at = d->size();
        d->resize(at + size, 0x3D);
        if (offset == 0) {
            std::memcpy(d->data() + at, &id, 4);
        }
    }
    /// The channel's reliable sequence is brought to `to` with traffic of the room, not by writing the field: small commands in order, 64 to a datagram, all
    /// taken. The scene then starts there (`base`), with nothing acknowledged, nothing handed over and the counters at zero.
    void AdvanceTo(u16 to) {
        for (int datagrams = 0; datagrams < 4000 && cp->channels[0].incomingReliableSequenceNumber != to; ++datagrams) {
            base = cp->channels[0].incomingReliableSequenceNumber;
            std::vector<u8> d = Header(0);
            u32 n = 0;
            while (n < 64 && static_cast<u16>(base + n) != to) {
                Reliable(&d, ++n, 4);
            }
            Send(d, static_cast<int>(n)); // (a datagram the loopback lost is made again from where the channel is)
        }
        Quiet();
        base = cp->channels[0].incomingReliableSequenceNumber;
        got.clear();
        g_acked.clear();
        peak = 0;
        ENetAzlCountersTake();
    }
    /// The datagram goes from the server's socket to the client's, and the client reads it (handing nothing over: `take` 0) or services.
    void Send(const std::vector<u8>& d, int take = 0) {
        ENetBuffer buffer;
        buffer.data = const_cast<u8*>(d.data());
        buffer.dataLength = d.size();
        CHECK(__real_enet_socket_send(server->socket, &client_address, &buffer, 1) == static_cast<int>(d.size()));
        // (the loopback delivers at once; a service call with no time reads it)
        if (take == 0) {
            enet_host_service(client, nullptr, 0); // reads and acknowledges, hands nothing over
        } else {
            Service(take);
        }
        if (cp->totalWaitingData > peak) {
            peak = cp->totalWaitingData;
        }
    }
    void One(u32 n, u32 size, int take = 0, u16 sent_time = 0) {
        std::vector<u8> d = Header(sent_time);
        Reliable(&d, n, size);
        Send(d, take);
    }
    /// The acknowledgements the client owes are out (the next call sends them) and read back.
    bool Acked(u32 n) {
        enet_host_service(client, nullptr, 0);
        return g_acked.count(static_cast<u16>(base + n)) != 0;
    }
    /// The numbers 2 .. until the data that waits is at the limit: all of it behind the hole at 1. Returns the last number queued.
    u32 FillBehindTheHole(u32 size) {
        u32 n = 1;
        while (cp->totalWaitingData < kLimit) {
            One(++n, size);
        }
        return n;
    }
};

bool InOrderFrom1(const std::vector<u32>& got, u32 last) {
    if (got.size() != last) {
        return false;
    }
    for (u32 i = 0; i < last; ++i) {
        if (got[i] != i + 1) {
            return false;
        }
    }
    return true;
}

// The state the console was in, and the way out. Before the remedy: the last check block fails (nothing is ever handed over again).
void TestTheWayOut() {
    std::printf("the limit reached behind a hole: the command that closes the hole is taken, acknowledged, and everything is handed over in order\n");
    Rig r;
    const u32 last = r.FillBehindTheHole(1200);
    std::printf("  %u commands of 1200 B wait behind the hole: %zu B (limit %zu)\n", last - 1, r.cp->totalWaitingData, kLimit);
    CHECK(r.cp->totalWaitingData >= kLimit);
    CHECK(r.cp->totalWaitingData < kLimit + 1200);
    r.Service();
    CHECK_EQ(r.got.size(), std::size_t(0)); // nothing is ready: all of it is behind number 1
    CHECK(r.Acked(last));                   // (what was queued was acknowledged: the room will never send it again)
    // more of the stream: refused, and NOT acknowledged (the room keeps them and sends them again)
    r.One(last + 1, 1200);
    r.One(last + 2, 1200);
    CHECK(!r.Acked(last + 1));
    CHECK(!r.Acked(last + 2));
    r.Service();
    CHECK_EQ(r.got.size(), std::size_t(0));
    // the room's retransmission of number 1 arrives
    r.One(1, 1200);
    CHECK(r.Acked(1));
    r.Service();
    CHECK_EQ(r.got.size(), std::size_t(last));
    CHECK(InOrderFrom1(r.got, last));
    CHECK_EQ(r.cp->totalWaitingData, std::size_t(0));
    CHECK(r.peak < kLimit + 2 * 1200); // (one command over the limit, the one that closed the hole)
    // and the two that were refused come again and are taken
    r.One(last + 1, 1200, 1);
    r.One(last + 2, 1200, 1);
    CHECK(r.Acked(last + 2));
    CHECK(InOrderFrom1(r.got, last + 2));
}

#ifdef ENET_AZL_LAG_LEAK_MS

void TestCounters() {
    std::printf("the counters: commands refused at the limit, commands taken over it, the peak of the waiting data\n");
    Rig r;
    ENetAzlCounters c;
    r.One(1, 600, 1);
    r.One(2, 600, 1);
    enet_azl_counters_take(r.client, &c);
    CHECK_EQ(c.capRefused, 0u); // a stream that flows never sees the limit
    CHECK_EQ(c.capAdmitted, 0u);
    CHECK_EQ(c.waitingMaxBytes, 600u);
    r.base = static_cast<u16>(r.base + 2);
    r.got.clear();
    const u32 last = r.FillBehindTheHole(1200);
    enet_azl_counters_take(r.client, &c);
    CHECK_EQ(c.capRefused, 0u);
    CHECK(c.waitingMaxBytes >= kLimit && c.waitingMaxBytes < kLimit + 1200);
    r.One(last + 1, 1200);
    r.One(last + 2, 1200);
    r.One(last + 3, 1200);
    enet_azl_counters_take(r.client, &c);
    CHECK_EQ(c.capRefused, 3u);
    CHECK_EQ(c.capAdmitted, 0u);
    r.One(1, 1200);
    enet_azl_counters_take(r.client, &c);
    CHECK_EQ(c.capRefused, 0u);
    CHECK_EQ(c.capAdmitted, 1u);
    CHECK(c.waitingMaxBytes >= kLimit + 1200 && c.waitingMaxBytes < kLimit + 2 * 1200);
    r.Service();
    CHECK(InOrderFrom1(r.got, last));
}

// What is refused stays refused: the limit is not gone for a stream that comes in order while the caller takes nothing.
void TestTheLimitHolds() {
    std::printf("the limit holds: an in-order stream the caller does not take is refused at it; a wide hole is closed up to one largest packet over it\n");
    // 1. In order, the caller takes nothing: everything is ready, nothing waits behind a hole, so nothing is taken over the limit.
    {
        Rig r;
        u32 n = 0;
        while (r.cp->totalWaitingData < kLimit) {
            r.One(++n, 1200);
        }
        const u32 full = n;
        for (int i = 0; i < 50; ++i) {
            r.One(++n, 1200);
        }
        CHECK(r.cp->totalWaitingData < kLimit + 1200);
        CHECK(!r.Acked(full + 1));
        ENetAzlCounters c;
        enet_azl_counters_take(r.client, &c);
        CHECK_EQ(c.capRefused, 50u);
        CHECK_EQ(c.capAdmitted, 0u);
        r.Service(); // the caller takes them: there is room again
        CHECK(InOrderFrom1(r.got, full));
        r.One(full + 1, 1200, 1);
        CHECK(InOrderFrom1(r.got, full + 1));
    }
    // 2. A hole of 100 commands (120 KB) in front of 192 KB that wait, the caller takes nothing: the commands of the hole are taken one after the other
    //    until the data is one largest packet over the limit, the rest is refused; never more than limit + 2 largest packets.
    {
        Rig r;
        constexpr u32 kHole = 100;
        u32 n = kHole;
        while (r.cp->totalWaitingData < kLimit) {
            r.One(++n, 1200);
        }
        const u32 last = n;
        u32 taken = 0;
        for (u32 h = 1; h <= kHole; ++h) {
            const std::size_t before = r.cp->totalWaitingData;
            r.One(h, 1200);
            if (r.cp->totalWaitingData == before) {
                break; // refused: the ones after it are not the next of the sequence
            }
            ++taken;
        }
        std::printf("  a hole of %u commands: %u taken over the limit, the data that waits %zu B (limit + %zu)\n", kHole, taken, r.cp->totalWaitingData,
                    r.cp->totalWaitingData - kLimit);
        CHECK(taken >= 50 && taken < kHole);                       // 64 KB of 1200 B commands
        CHECK(r.cp->totalWaitingData >= kLimit + kPacketMax);       // it stopped at the bound ...
        CHECK(r.peak < kLimit + kPacketMax + 2 * 1200);             // ... and not later
        CHECK(r.peak < kLimit + 2 * kPacketMax);                    // (the bound of the comment in enet.h)
        // the caller takes what is ready; the room sends the rest of the hole again; everything arrives, in order
        for (u32 h = taken + 1; h <= kHole; ++h) {
            r.Service();
            r.One(h, 1200);
        }
        r.Service();
        CHECK(InOrderFrom1(r.got, last));
        CHECK_EQ(r.cp->totalWaitingData, std::size_t(0));
    }
    // 3. A command that is not the next of the sequence is refused at the limit even inside the hole (the hole is closed from its start).
    {
        Rig r;
        u32 n = 3;
        while (r.cp->totalWaitingData < kLimit) {
            r.One(++n, 1200);
        }
        const std::size_t before = r.cp->totalWaitingData;
        r.One(2, 1200);
        r.One(3, 1200);
        CHECK_EQ(r.cp->totalWaitingData, before);
        CHECK(!r.Acked(2));
        r.One(1, 1200);
        r.One(2, 1200);
        r.One(3, 1200);
        r.Service();
        CHECK(InOrderFrom1(r.got, n));
    }
}

// ENet dropped the rest of a datagram at a command it refused: the command that closes the hole may be in the same datagram, after one that is refused
// (the room sends again, in one datagram, whatever timed out together).
void TestTheDatagramIsReadOn() {
    std::printf("a datagram is read on after a command that was refused at the limit: the one that closes the hole comes after it\n");
    Rig r;
    const u32 last = r.FillBehindTheHole(600);
    std::vector<u8> d = r.Header(0);
    r.Reliable(&d, last + 1, 300); // refused: not the next of the sequence
    r.Reliable(&d, 1, 300);        // the one that closes the hole
    r.Send(d);
    CHECK(!r.Acked(last + 1));
    CHECK(r.Acked(1));
    r.Service();
    CHECK(InOrderFrom1(r.got, last));
    ENetAzlCounters c;
    enet_azl_counters_take(r.client, &c);
    CHECK_EQ(c.capRefused, 1u);
    CHECK_EQ(c.capAdmitted, 1u);
    // (a datagram with a malformed command is still dropped from there on, as ever: the second command is never seen)
    std::vector<u8> bad = r.Header(0);
    bad.push_back(ENET_PROTOCOL_COMMAND_SEND_RELIABLE | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE);
    bad.push_back(9); // a channel the connection does not have
    Rig::Put16(&bad, static_cast<u16>(r.base + last + 1));
    Rig::Put16(&bad, 4);
    bad.insert(bad.end(), 4, 0);
    r.Reliable(&bad, last + 1, 300);
    r.Send(bad, 1);
    CHECK(!r.Acked(last + 1));
    CHECK_EQ(r.got.size(), std::size_t(last));
}

// Reading on is for a command refused at the limit and for nothing else. The scene above cannot tell (its malformed command names a channel that is
// not there: the handler leaves before it has stepped over the command's data, and the reader stops on those bytes whatever the rule is). Here the
// command that fails has been stepped over - a fragment whose number is beyond its packet's count - so what decides is the rule alone: the valid
// command behind it in the datagram must NOT be read, with the stream flowing and right after a refusal at the limit (the mark of a refusal is
// the previous command's, never the datagram's or the connection's).
void TestAnyOtherFailureDropsTheDatagram() {
    std::printf("a command that fails for another reason drops the rest of its datagram, as ever: with the stream flowing, and right after a refusal at the limit\n");
    const auto bad_fragment = [](Rig& r, std::vector<u8>* d, u32 first) { r.Fragment(d, first, 5, 2, 2000, 0, 100); }; // fragment 5 of 2: refused by its handler
    // 1. the stream flows, the limit is far
    {
        Rig r;
        std::vector<u8> d = r.Header(0);
        bad_fragment(r, &d, 1);
        r.Reliable(&d, 1, 300); // valid, and the next of the sequence: not read
        r.Send(d, 1);
        CHECK(!r.Acked(1));
        CHECK_EQ(r.got.size(), std::size_t(0));
        CHECK_EQ(r.cp->totalWaitingData, std::size_t(0));
        ENetAzlCounters c;
        enet_azl_counters_take(r.client, &c);
        CHECK_EQ(c.capRefused, 0u);
        r.One(1, 300, 1); // (alone it is taken)
        CHECK(r.Acked(1));
        CHECK_EQ(r.got.size(), std::size_t(1));
    }
    // 2. at the limit behind a hole, a command has just been refused (in a datagram of its own); the NEXT datagram has a command that fails for another
    //    reason and, behind it, the one that closes the hole: dropped at the failure, the hole stays
    {
        Rig r;
        const u32 last = r.FillBehindTheHole(600);
        r.One(last + 1, 300); // refused at the limit
        CHECK(!r.Acked(last + 1));
        std::vector<u8> d = r.Header(0);
        bad_fragment(r, &d, last + 5);
        r.Reliable(&d, 1, 300);
        r.Send(d);
        CHECK(!r.Acked(1));
        r.Service();
        CHECK_EQ(r.got.size(), std::size_t(0));
        ENetAzlCounters c;
        enet_azl_counters_take(r.client, &c);
        CHECK_EQ(c.capRefused, 1u);
        CHECK_EQ(c.capAdmitted, 0u);
        // 3. the same in ONE datagram: refused at the limit (read on), then the failure (dropped from there), then the one that closes the hole (not read)
        std::vector<u8> e = r.Header(0);
        r.Reliable(&e, last + 1, 300);
        bad_fragment(r, &e, last + 5);
        r.Reliable(&e, 1, 300);
        r.Send(e);
        CHECK(!r.Acked(1));
        enet_azl_counters_take(r.client, &c);
        CHECK_EQ(c.capRefused, 1u);
        CHECK_EQ(c.capAdmitted, 0u);
        // (the room sends the one that closes the hole again, alone: everything arrives)
        r.One(1, 300);
        CHECK(r.Acked(1));
        r.Service();
        CHECK(InOrderFrom1(r.got, last));
    }
}

// The "next of the sequence" is a 16-bit number that wraps: the hole at number 0x0000 (the channel stands at 0xFFFF) is closed like any other, by a
// command and by a packet in fragments. (A "next" worked out without the wrap is 0x10000, which no command has: the dead end again, once in 65536.)
void TestTheHoleAtTheWrap() {
    std::printf("the sequence wraps: the hole at number 0x0000 is closed by one command, and by a packet in fragments\n");
    {
        Rig r;
        r.AdvanceTo(0xFFFF);
        CHECK_EQ(u32(r.cp->channels[0].incomingReliableSequenceNumber), 0xFFFFu);
        const u32 last = r.FillBehindTheHole(1200); // 0x0001 .. behind the hole at 0x0000
        r.One(last + 1, 1200);
        CHECK(!r.Acked(last + 1));
        r.One(1, 1200); // number 0x0000
        CHECK(r.Acked(1));
        r.Service();
        CHECK(InOrderFrom1(r.got, last));
        CHECK_EQ(r.cp->totalWaitingData, std::size_t(0));
        CHECK_EQ(u32(r.cp->channels[0].incomingReliableSequenceNumber), last - 1);
    }
    {
        Rig r;
        r.AdvanceTo(0xFFFF);
        // numbers 0x0000 and 0x0001 are the two fragments of one packet of 2000 B; 0x0002 .. wait behind it
        u32 n = 2;
        while (r.cp->totalWaitingData < kLimit) {
            r.One(++n, 1200);
        }
        std::vector<u8> first = r.Header(0);
        r.Fragment(&first, 1, 0, 2, 2000, 0, 1300);
        r.Send(first);
        CHECK(r.Acked(1));
        std::vector<u8> second = r.Header(0);
        r.Fragment(&second, 1, 1, 2, 2000, 1300, 700);
        r.Send(second);
        CHECK(r.Acked(2));
        r.Service();
        CHECK_EQ(r.got.size(), std::size_t(n - 1));
        CHECK(r.got.size() == n - 1 && r.got[0] == 1 && r.got[1] == 3 && r.got.back() == n);
        CHECK_EQ(r.cp->totalWaitingData, std::size_t(0));
    }
}

// Unreliable commands and the limit. The room sends none (every packet of an Azahar room is reliable), so this is the rule's edge, not the console's day:
// an unreliable command is never taken over the limit, whatever its numbers say. (Unreliable commands that fill the limit behind a missing reliable one
// are NOT a case the rule claims - see ENET_AZL_HOLE_CLOSER in peer.c; that they are handed over when that reliable one comes is TestUnreliableBehindAHole.)
void TestUnreliableCommandsAtTheLimit() {
    std::printf("unreliable commands: never taken over the limit\n");
    {
        Rig r;
        r.FillBehindTheHole(1200);
        const std::size_t before = r.cp->totalWaitingData;
        std::vector<u8> d = r.Header(0);
        r.Unreliable(&d, 1, 1, 7001, 300); // "the next of the sequence" by its reliable number, commands wait, the bound is far: and still not taken
        r.Send(d);
        CHECK_EQ(r.cp->totalWaitingData, before);
        ENetAzlCounters c;
        enet_azl_counters_take(r.client, &c);
        CHECK_EQ(c.capRefused, 1u);
        CHECK_EQ(c.capAdmitted, 0u);
    }
}

// (2026-11-15, E1 of the plan) An unreliable command that waits for a missing reliable one is handed over when that reliable one comes, right after it, as in
// upstream ENet. Our rework of the hand-over (2026-10-25) had made the test at the end of enet_peer_dispatch_incoming_reliable_commands always false: such a
// command stayed (and counted against the limit) until the next unreliable command. The room sends none today: this is the edge of ENet, not the console's day.
void TestUnreliableBehindAHole() {
    std::printf("an unreliable command behind a missing reliable one: handed over when the hole closes\n");
    {   // reliable 2, then an unreliable one after 2, then reliable 1: 1, 2 and the unreliable one, in that order, nothing left waiting
        Rig r;
        r.One(2, 300);
        std::vector<u8> d = r.Header(0);
        r.Unreliable(&d, 2, 1, 9001, 200);
        r.Send(d);
        CHECK(r.cp->totalWaitingData > 0);
        r.One(1, 300);
        r.Service();
        CHECK(r.got.size() == 3 && r.got[0] == 1 && r.got[1] == 2 && r.got[2] == 9001);
        CHECK_EQ(r.cp->totalWaitingData, std::size_t(0));
        CHECK(enet_list_empty(&r.cp->channels[0].incomingUnreliableCommands));
    }
    {   // the same with an unreliable packet in two fragments
        Rig r;
        r.One(2, 300);
        std::vector<u8> d = r.Header(0);
        r.UnreliableFragment(&d, 2, 1, 0, 2, 1500, 0, 1000, 9002);
        r.UnreliableFragment(&d, 2, 1, 1, 2, 1500, 1000, 500, 9002);
        r.Send(d);
        r.One(1, 300);
        r.Service();
        CHECK(r.got.size() == 3 && r.got[0] == 1 && r.got[1] == 2 && r.got[2] == 9002);
        CHECK_EQ(r.cp->totalWaitingData, std::size_t(0));
        CHECK(enet_list_empty(&r.cp->channels[0].incomingUnreliableCommands));
    }
    {   // an unreliable one after 1 while 1 and 2 are missing: when both come it is behind the sequence and goes away, nothing left waiting
        Rig r;
        std::vector<u8> d = r.Header(0);
        r.Unreliable(&d, 1, 1, 9003, 200);
        r.Send(d);
        r.One(2, 300);
        r.One(1, 300);
        r.Service();
        CHECK(r.got.size() == 2 && r.got[0] == 1 && r.got[1] == 2); // (behind the sequence: dropped, as upstream)
        CHECK_EQ(r.cp->totalWaitingData, std::size_t(0));
        CHECK(enet_list_empty(&r.cp->channels[0].incomingUnreliableCommands));
    }
    {   // with the drop at the limit on: the unreliable one waits, the limit fills behind the hole, the hole closes; nothing left, nothing read after a free
        Rig r;
#ifdef ENET_AZL_CAP_DROP_MAX
        r.client->azlCapDrop = [](const enet_uint8*, size_t) -> int { return 1; };
#endif
        std::vector<u8> d = r.Header(0);
        r.Unreliable(&d, 2, 1, 9004, 200);
        r.Send(d);
        const u32 last = r.FillBehindTheHole(1200);
        r.One(1, 1200);
        r.Service();
        CHECK(InOrderFrom1(r.got, last)); // (the one that closes the hole is taken, then 2 .. last; the unreliable one is behind them: dropped)
        CHECK_EQ(r.cp->totalWaitingData, std::size_t(0));
        CHECK(enet_list_empty(&r.cp->channels[0].incomingUnreliableCommands));
    }
}

// The packet that closes the hole is a large one, in fragments (a UDS frame above ENet's fragment length comes as two).
void TestALargePacketClosesTheHole() {
    std::printf("the packet that closes the hole comes in fragments: its first fragment is taken over the limit, the others complete it\n");
    Rig r;
    // numbers 1 and 2 are the two fragments of one packet of 2000 B; 3 .. wait behind it
    u32 n = 2;
    while (r.cp->totalWaitingData < kLimit) {
        r.One(++n, 1200);
    }
    std::vector<u8> second = r.Header(0);
    r.Fragment(&second, 1, 1, 2, 2000, 1300, 700); // the second fragment first (the first was lost again)
    r.Send(second);
    CHECK(r.Acked(2));
    CHECK(r.cp->totalWaitingData >= kLimit + 2000);
    r.Service();
    CHECK_EQ(r.got.size(), std::size_t(0)); // not whole yet
    // a third command of the stream meanwhile: refused
    r.One(n + 1, 1200);
    CHECK(!r.Acked(n + 1));
    std::vector<u8> first = r.Header(0);
    r.Fragment(&first, 1, 0, 2, 2000, 0, 1300);
    r.Send(first);
    CHECK(r.Acked(1));
    r.Service();
    // the packet of numbers 1-2 (it says 1), then 3 ..
    CHECK_EQ(r.got.size(), std::size_t(n - 1));
    CHECK(r.got.size() == n - 1 && r.got[0] == 1 && r.got[1] == 3 && r.got.back() == n);
    CHECK_EQ(r.cp->totalWaitingData, std::size_t(0));
}

// The lag of the stream: how late a packet reaches the caller, from the sender's clock in the header of its datagram. (The server of the rig says nothing
// in this scene - it is not serviced and the client does not ping it - so the only sender's clock the client sees is the one written here: the virtual
// clock at the moment the "room" sent, which is also the clock of the handshake's datagrams.)
void TestStreamLag() {
    std::printf("the lag of the stream: a datagram's delay against the best one seen, plus the wait behind a hole or behind the caller\n");
    Rig r;
    enet_peer_ping_interval(r.cp, 0x7FFFFFFF);
    ENetAzlCounters c;
    const auto at = [](enet_uint32 clock) { return static_cast<u16>(clock & 0xFFFF); };
    // sent and read in the same millisecond, twice, 100 ms apart: no lag
    r.One(1, 100, 1, at(g_clock));
    g_clock += 100;
    r.One(2, 100, 1, at(g_clock));
    enet_azl_counters_take(r.client, &c);
    CHECK_EQ(c.streamLagMaxMs, 0u);
    // one that was 250 ms on the way and in the socket (the thread did not read, the WiFi held it)
    g_clock += 250;
    r.One(3, 100, 1, at(g_clock - 250));
    enet_azl_counters_take(r.client, &c);
    CHECK_EQ(c.streamLagMaxMs, 250u);
    // a hole: 5 arrives in time, 4 comes 400 ms later (the room's retransmission): 5 reaches the caller 400 ms late
    g_clock += 100;
    r.One(5, 100, 1, at(g_clock));
    enet_azl_counters_take(r.client, &c);
    CHECK_EQ(c.streamLagMaxMs, 0u); // (nothing handed over)
    CHECK_EQ(r.got.size(), std::size_t(3));
    g_clock += 400;
    r.One(4, 100, 2, at(g_clock));
    enet_azl_counters_take(r.client, &c);
    CHECK_EQ(c.streamLagMaxMs, 400u);
    CHECK_EQ(c.holWaitMaxMs, 400u); // (the same wait, as the older counter has it)
    // a datagram that was late AND waited behind a hole: the two add up
    g_clock += 100;
    r.One(7, 100, 1, at(g_clock - 60));
    g_clock += 200;
    r.One(6, 100, 2, at(g_clock));
    enet_azl_counters_take(r.client, &c);
    CHECK_EQ(c.streamLagMaxMs, 260u);
    // the caller is slow: two packets read in one pass, the second taken 30 ms later
    g_clock += 100;
    {
        std::vector<u8> d = r.Header(at(g_clock));
        r.Reliable(&d, 8, 100);
        r.Reliable(&d, 9, 100);
        r.Send(d, 1);
    }
    g_clock += 30;
    r.Service(1);
    enet_azl_counters_take(r.client, &c);
    CHECK_EQ(c.streamLagMaxMs, 30u);
    CHECK_EQ(r.got.size(), std::size_t(9));
    // a datagram that is EARLIER than the best so far (the room's clock is 20 ms ahead of what the handshake said) moves the base: a lag is never negative,
    // and the next ones are measured against it
    g_clock += 100;
    r.One(10, 100, 1, at(g_clock + 20));
    enet_azl_counters_take(r.client, &c);
    CHECK_EQ(c.streamLagMaxMs, 0u);
    g_clock += 100;
    r.One(11, 100, 1, at(g_clock));
    enet_azl_counters_take(r.client, &c);
    CHECK_EQ(c.streamLagMaxMs, 20u);
    // two clocks that drift apart are not a lag: the console's runs 1 ms in 4097 fast for 41 s (10 ms in all); the base gives way by 1 ms every 4096
    enet_uint32 room = g_clock + 20;
    u32 n = 11;
    for (int i = 0; i < 10; ++i) {
        g_clock += 4097;
        room += 4096;
        r.One(++n, 100, 1, at(room));
    }
    enet_azl_counters_take(r.client, &c);
    std::printf("  10 ms of drift in 41 s: the largest lag shown is %u ms\n", c.streamLagMaxMs);
    CHECK(c.streamLagMaxMs <= 2u);
    // ... and the 16 bits of the header's clock wrap without a trace (80 s)
    for (int i = 0; i < 20; ++i) {
        g_clock += 4000;
        room += 4000;
        r.One(++n, 100, 1, at(room));
    }
    enet_azl_counters_take(r.client, &c);
    CHECK(c.streamLagMaxMs <= 2u);
    // a real lag after all that is still told
    g_clock += 4000 + 700;
    room += 4000;
    r.One(++n, 100, 1, at(room));
    enet_azl_counters_take(r.client, &c);
    CHECK(c.streamLagMaxMs >= 698u && c.streamLagMaxMs <= 702u);
    CHECK_EQ(r.got.size(), std::size_t(n));
    CHECK(InOrderFrom1(r.got, n));
}

// (plugin-causa, 2026-11-14 22:50: the minigames of Mario Party: Island Tour hosted by a console.) What a refusal costs: it is not acknowledged, so the
// room's timer of that command doubles, and the refused commands are the ones the stream needs right after the hole - the next hole lasts as long as
// their timers (2.9, 6.4, 8.7 s in that session) until one of them has gone 6 times and the room ends the link. For the listed titles the room client
// gives ENet a filter of game frames (ENetHost::azlCapDrop): at the limit such a frame is ACKNOWLEDGED and dropped - its place in the sequence kept by a
// placeholder - and never refused. The room's messages (here: data that starts with kControl) are refused as before, and so is a fragment that is not
// its packet's first (its packet cannot be told). On a tree without azlCapDrop the first "Acked" checks fail: there everything is refused.
void TestTheDropAtTheLimit() {
    std::printf("the listed titles at the limit: a game frame is acknowledged and dropped (its place kept), a room message and an unknown fragment refused\n");
    constexpr u32 kControl = 0xC0DE0000u;
    Rig r;
#ifdef ENET_AZL_CAP_DROP_MAX
    r.client->azlCapDrop = [](const enet_uint8* data, size_t length) -> int {
        u32 id = 0;
        if (length >= 4) {
            std::memcpy(&id, data, 4);
        }
        return id != kControl ? 1 : 0;
    };
#endif
    const u32 last = r.FillBehindTheHole(1200);
    const std::size_t held = r.cp->totalWaitingData;
    // a frame of the stream: acknowledged, nothing more waits
    r.One(last + 1, 1200);
    CHECK(r.Acked(last + 1));
    CHECK_EQ(r.cp->totalWaitingData, held);
    // a frame of 2900 B in three fragments (numbers last+2 .. last+4): its second fragment first - refused (the packet cannot be told) ...
    {
        std::vector<u8> d = r.Header(0);
        r.Fragment(&d, last + 2, 1, 3, 2900, 1300, 1300);
        r.Send(d);
        CHECK(!r.Acked(last + 3));
        // ... then its first: dropped and acknowledged, and the two others are acknowledged on the placeholder
        std::vector<u8> f0 = r.Header(0);
        r.Fragment(&f0, last + 2, 0, 3, 2900, 0, 1300);
        r.Send(f0);
        CHECK(r.Acked(last + 2));
        std::vector<u8> f1 = r.Header(0);
        r.Fragment(&f1, last + 2, 1, 3, 2900, 1300, 1300);
        r.Fragment(&f1, last + 2, 2, 3, 2900, 2600, 300);
        r.Send(f1);
        CHECK(r.Acked(last + 3));
        CHECK(r.Acked(last + 4));
        CHECK_EQ(r.cp->totalWaitingData, held);
    }
    // a message of the room: refused as ever, not acknowledged
    {
        std::vector<u8> d = r.Header(0);
        r.Reliable(&d, last + 5, 600);
        std::memcpy(d.data() + d.size() - 600, &kControl, 4);
        r.Send(d);
        CHECK(!r.Acked(last + 5));
    }
    r.One(last + 6, 1200); // another frame: dropped
    CHECK(r.Acked(last + 6));
#ifdef ENET_AZL_CAP_DROP_MAX
    CHECK_EQ(r.client->azlCapDropHeld, 3u); // last+1, the packet of last+2..4, last+6
    {
        ENetAzlCounters c;
        enet_azl_counters_take(r.client, &c);
        CHECK_EQ(c.capDropped, 3u);
        CHECK_EQ(c.capRefused, 2u); // the unknown fragment and the room's message
    }
#endif
    // the room's retransmission of number 1: everything that was kept is handed over in order, up to the room's message (refused, so a hole)
    r.One(1, 1200);
    r.Service();
    CHECK(InOrderFrom1(r.got, last));
    CHECK_EQ(r.cp->totalWaitingData, std::size_t(0));
    // the room's message again: taken (below the limit now), handed over; the placeholder behind it goes, nothing is handed over for it
    {
        std::vector<u8> d = r.Header(0);
        r.Reliable(&d, last + 5, 600);
        std::memcpy(d.data() + d.size() - 600, &kControl, 4);
        r.Send(d, 1);
        CHECK(r.Acked(last + 5));
    }
    r.Service();
    CHECK_EQ(r.got.size(), std::size_t(last + 1));
    CHECK(r.got.size() == last + 1 && r.got.back() == kControl);
    CHECK_EQ(r.cp->channels[0].incomingReliableSequenceNumber, static_cast<u16>(r.base + last + 6));
#ifdef ENET_AZL_CAP_DROP_MAX
    CHECK_EQ(r.client->azlCapDropHeld, 0u);
#endif
    // late copies of what was dropped: duplicates, acknowledged, never handed over; the stream goes on
    g_acked.clear();
    r.One(last + 1, 1200);
    r.One(last + 6, 1200);
    CHECK(r.Acked(last + 6));
    r.One(last + 7, 1200, 1);
    r.Service();
    CHECK(r.got.size() == last + 2 && r.got.back() == last + 7);
}

// (2026-11-15, the review of the drop.) A frame dropped at the limit that is the NEXT of the sequence, with nothing waiting in the channel's list: the limit
// is held by packets already in order that the caller has not taken yet (a slow network thread). Its placeholder is in order at once, so the dispatch
// frees it in the same call; the fragment's handler must not read it after that (it did: enet_peer_queue_incoming_command returned the freed placeholder,
// and with the address sanitizer of this build that read stops the test). The frame and its other fragments are acknowledged, nothing of it is handed
// over, and the stream goes on.
void TestTheDropInOrder() {
    std::printf("a frame dropped at the limit that is next in order (the limit held by packets not taken yet): acknowledged, freed, never read again\n");
    Rig r;
#ifdef ENET_AZL_CAP_DROP_MAX
    r.client->azlCapDrop = [](const enet_uint8*, size_t) -> int { return 1; };
#endif
    // 1 .. last in order, none taken: they wait in the peer's dispatched list, the channel's list is empty
    u32 last = 0;
    while (r.cp->totalWaitingData < kLimit) {
        r.One(++last, 1200);
    }
    CHECK(enet_list_empty(&r.cp->channels[0].incomingReliableCommands));
    const std::size_t held = r.cp->totalWaitingData;
    // the first fragment of a frame of 2900 B (numbers last+1 .. last+3): dropped in order, then its two other fragments
    std::vector<u8> f0 = r.Header(0);
    r.Fragment(&f0, last + 1, 0, 3, 2900, 0, 1300);
    r.Send(f0);
    CHECK(r.Acked(last + 1));
    std::vector<u8> f1 = r.Header(0);
    r.Fragment(&f1, last + 1, 1, 3, 2900, 1300, 1300);
    r.Fragment(&f1, last + 1, 2, 3, 2900, 2600, 300);
    r.Send(f1);
    CHECK(r.Acked(last + 2));
    CHECK(r.Acked(last + 3));
    // a whole frame next in order: dropped and freed the same way
    r.One(last + 4, 1200);
    CHECK(r.Acked(last + 4));
    CHECK_EQ(r.cp->totalWaitingData, held);
    CHECK_EQ(r.cp->channels[0].incomingReliableSequenceNumber, static_cast<u16>(r.base + last + 4));
#ifdef ENET_AZL_CAP_DROP_MAX
    CHECK_EQ(r.client->azlCapDropHeld, 0u); // (both freed at once: nothing held)
    {
        ENetAzlCounters c;
        enet_azl_counters_take(r.client, &c);
        CHECK_EQ(c.capDropped, 2u);
        CHECK_EQ(c.capRefused, 0u);
    }
#endif
    // the caller takes what waited; the next frame is taken and handed over after it
    r.Service();
    CHECK(InOrderFrom1(r.got, last));
    r.One(last + 5, 1200, 1);
    r.Service();
    CHECK(r.got.size() == last + 1 && r.got.back() == last + 5);
    CHECK_EQ(r.cp->totalWaitingData, std::size_t(0));
}

// The bound of the placeholders (azlCapDropMax): beyond it a frame is refused as before; the place of each one is freed when its turn comes.
void TestTheDropIsBounded() {
    std::printf("the placeholders of the dropped frames are bounded: beyond azlCapDropMax a frame is refused as before\n");
    Rig r;
#ifdef ENET_AZL_CAP_DROP_MAX
    r.client->azlCapDrop = [](const enet_uint8*, size_t) -> int { return 1; };
    r.client->azlCapDropMax = 4;
#endif
    const u32 last = r.FillBehindTheHole(1200);
    for (u32 i = 1; i <= 6; ++i) {
        r.One(last + i, 1200);
    }
    CHECK(r.Acked(last + 4));
    CHECK(!r.Acked(last + 5));
    CHECK(!r.Acked(last + 6));
#ifdef ENET_AZL_CAP_DROP_MAX
    CHECK_EQ(r.client->azlCapDropHeld, 4u);
#endif
#ifdef ENET_AZL_AWAY_MS
    {
        // the counters say why the last two were refused: the bound, not the filter (the line "room stream losses")
        ENetAzlCounters c;
        enet_azl_counters_take(r.client, &c);
        CHECK_EQ(c.capDropped, 4u);
        CHECK_EQ(c.capRefused, 2u);
        CHECK_EQ(c.capDropFull, 2u);
        CHECK_EQ(c.capDropHeldMax, 4u);
        CHECK_EQ(c.seqSkipped, 1u); // (number 1, the hole: never seen)
        enet_azl_counters_take(r.client, &c);
        CHECK_EQ(c.capDropHeldMax, 4u); // a window starts from what is held
    }
#endif
    r.One(1, 1200);
    r.Service();
    CHECK(InOrderFrom1(r.got, last));
#ifdef ENET_AZL_CAP_DROP_MAX
    CHECK_EQ(r.client->azlCapDropHeld, 0u); // the four went with the run they were in
#endif
    r.One(last + 5, 1200, 1);
    r.One(last + 6, 1200, 1);
    CHECK(r.got.size() == last + 2 && r.got[last] == last + 5 && r.got[last + 1] == last + 6);
    // and a reset of the peer with placeholders in the list leaves the count at zero
    {
        Rig q;
#ifdef ENET_AZL_CAP_DROP_MAX
        q.client->azlCapDrop = [](const enet_uint8*, size_t) -> int { return 1; };
#endif
        const u32 l = q.FillBehindTheHole(1200);
        q.One(l + 1, 1200);
        q.One(l + 2, 1200);
#ifdef ENET_AZL_CAP_DROP_MAX
        CHECK_EQ(q.client->azlCapDropHeld, 2u);
        enet_peer_reset(q.cp);
        CHECK_EQ(q.client->azlCapDropHeld, 0u);
#endif
    }
}

#ifdef ENET_AZL_AWAY_MS
// The numbers of the room's stream that never came (ENetAzlCounters::seqSkipped): a new command lost on the way. A copy sent again, a late one and the
// fragments of a packet are not counted; a number skipped in what waited in the socket while the thread was away (ENET_AZL_AWAY_MS) is counted apart.
void TestSkippedNumbers() {
    std::printf("the numbers of the room's stream that never came are counted, and apart the ones found in what waited while the thread was away\n");
    Rig r;
    r.One(1, 100, 1);
    r.One(2, 100, 1);
    r.One(5, 100, 1); // 3 and 4 never came
    r.One(3, 100, 1); // (they come late, sent again: not counted again)
    r.One(4, 100, 1);
    {
        std::vector<u8> d = r.Header(0); // a packet in three fragments, 6 .. 8: no number skipped
        r.Fragment(&d, 6, 0, 3, 2900, 0, 1300);
        r.Fragment(&d, 6, 1, 3, 2900, 1300, 1300);
        r.Fragment(&d, 6, 2, 3, 2900, 2600, 300);
        r.Send(d, 1);
    }
    r.One(9, 100, 1);
    r.One(9, 100, 1); // (a copy)
    ENetAzlCounters c;
    enet_azl_counters_take(r.client, &c);
    CHECK_EQ(c.seqSkipped, 2u);
    CHECK_EQ(c.seqSkippedAway, 0u);
    // the thread away 200 ms: number 10 skipped in what had waited; then the socket is found empty, and 12 skipped is not "away"
    g_clock += 200;
    r.One(11, 100, 1);
    r.One(13, 100, 1);
    enet_azl_counters_take(r.client, &c);
    CHECK_EQ(c.seqSkipped, 2u);
    CHECK_EQ(c.seqSkippedAway, 1u);
    r.One(10, 100, 1);
    r.One(12, 100, 1);
    r.Service();
    CHECK(r.got == std::vector<u32>({1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13})); // (6: the packet of 6 .. 8)
}

// (2026-11-15, N1 of the plan) What the socket's ring held after the thread was away: the datagrams and bytes read from the first read after it came back until
// the socket was found empty, and the copies the room sent again among them (ENetAzlCounters::away*, the line "room stream losses").
void TestAwayCounters() {
    std::printf("after the thread was away: what waited in the socket is counted up to the empty socket, with the copies the room sent again\n");
    Rig r;
    r.One(1, 100, 1);
    ENetAzlCounters c;
    enet_azl_counters_take(r.client, &c);
    auto raw = [&r](const std::vector<u8>& d) {   // into the client's socket, nothing read: the ring fills while the thread is away
        ENetBuffer buffer;
        buffer.data = const_cast<u8*>(d.data());
        buffer.dataLength = d.size();
        CHECK(__real_enet_socket_send(r.server->socket, &r.client_address, &buffer, 1) == static_cast<int>(d.size()));
    };
    g_clock += 200;
    std::size_t bytes = 0;
    for (u32 n = 2; n <= 5; ++n) {
        std::vector<u8> d = r.Header(0);
        r.Reliable(&d, n, 300);
        raw(d);
        bytes += d.size();
    }
    {
        std::vector<u8> d = r.Header(0); // a copy of 1, handled already: the room sent it again
        r.Reliable(&d, 1, 100);
        raw(d);
        bytes += d.size();
    }
    r.Service();
    CHECK(r.got == std::vector<u32>({1, 2, 3, 4, 5}));
    enet_azl_counters_take(r.client, &c);
    CHECK_EQ(c.awayEpisodes, 1u);
    CHECK_EQ(c.awayDatagramsMax, 5u);
    CHECK_EQ(c.awayBytesMax, static_cast<u32>(bytes));
    CHECK_EQ(c.awayDupMax, 1u);
    CHECK_EQ(c.dupCommands, 1u);
    // back in time, the socket found empty: the next datagram and its copy are not the ring's
    r.One(6, 100, 1);
    r.One(6, 100, 1);
    enet_azl_counters_take(r.client, &c);
    CHECK_EQ(c.awayEpisodes, 0u);
    CHECK_EQ(c.awayDatagramsMax, 0u);
    CHECK_EQ(c.awayBytesMax, 0u);
    CHECK_EQ(c.awayDupMax, 0u);
    CHECK_EQ(c.dupCommands, 1u);
    // away twice: two episodes, the maximum is the larger one
    g_clock += 150;
    r.One(7, 100, 1);
    g_clock += 150;
    for (u32 n = 8; n <= 10; ++n) {
        std::vector<u8> d = r.Header(0);
        r.Reliable(&d, n, 100);
        raw(d);
    }
    r.Service();
    enet_azl_counters_take(r.client, &c);
    CHECK_EQ(c.awayEpisodes, 2u);
    CHECK_EQ(c.awayDatagramsMax, 3u);
}
#endif

#endif // ENET_AZL_LAG_LEAK_MS

// What ENet does with frames that are late, with nothing added: the loop of the network thread takes ALL the game's frames that wait, gives each to ENet and
// flushes once (RoomClient::Run), and one flush fills every datagram to the connection's MTU with the commands that wait. So frames that have piled up
// already leave grouped, as many to a datagram as fit, with no wait - the grouping is bounded by the MTU and nothing else: eight frames of 124 bytes in their
// envelope to a datagram, two of 584. (Held here because the remedy of 2026-11-14 counts on it and adds nothing to it.)
void TestLateFramesShareADatagram() {
    std::printf("frames that wait go out together: one flush, as many to a datagram as the MTU takes (8 of 165 B, 2 of 625 B), and the acknowledgement rides along\n");
    Rig r;
    const u32 mtu = r.cp->mtu;
    CHECK(mtu >= 1392 && mtu <= 1400);
    const auto queue = [&r](int frames, std::size_t size) {
        std::vector<u8> data(size, 0x33);
        for (int i = 0; i < frames; ++i) {
            // (as RoomClient sends a broadcast frame of the game: EnetPacketFlags(false))
            enet_peer_send(r.cp, 0, enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_UNSEQUENCED | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT));
        }
    };
    // 25 small frames (124 B of the game + 41 of the room's envelope) that waited: 4 datagrams, not 25
    g_client_datagrams = g_client_largest = 0;
    queue(25, 165);
    enet_host_flush(r.client);
    CHECK_EQ(g_client_datagrams, 4u);
    CHECK(g_client_largest <= mtu);
    // 6 large ones (584 + 41): two to a datagram
    g_client_datagrams = g_client_largest = 0;
    queue(6, 625);
    enet_host_flush(r.client);
    CHECK_EQ(g_client_datagrams, 3u);
    CHECK(g_client_largest <= mtu && g_client_largest > 2 * 625);
    // one frame alone is one datagram: nothing waits for company
    g_client_datagrams = 0;
    queue(1, 165);
    enet_host_flush(r.client);
    CHECK_EQ(g_client_datagrams, 1u);
    // the acknowledgement of a datagram of the room goes in the same datagram as the frames
    r.One(1, 100, 1);
    g_client_datagrams = 0;
    g_acked.clear();
    queue(3, 165);
    enet_host_flush(r.client);
    CHECK_EQ(g_client_datagrams, 1u);
    CHECK(g_acked.count(static_cast<u16>(r.base + 1)) != 0);
}

} // namespace

int main() {
    CHECK_EQ(enet_initialize(), 0);
    TestTheWayOut();
    TestLateFramesShareADatagram();
#ifdef ENET_AZL_LAG_LEAK_MS
    TestCounters();
    TestTheLimitHolds();
    TestTheDatagramIsReadOn();
    TestAnyOtherFailureDropsTheDatagram();
    TestALargePacketClosesTheHole();
    TestTheHoleAtTheWrap();
    TestUnreliableCommandsAtTheLimit();
    TestUnreliableBehindAHole();
    TestStreamLag();
    TestTheDropAtTheLimit();
    TestTheDropInOrder();
    TestTheDropIsBounded();
#ifdef ENET_AZL_AWAY_MS
    TestSkippedNumbers();
    TestAwayCounters();
#endif
#else
    std::printf("(this tree has no remedy for the limit's dead end: only the scene that shows it was run)\n");
#endif
    enet_deinitialize();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
