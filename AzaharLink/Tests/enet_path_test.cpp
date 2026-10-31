// The data path of the ENet client (Sources/enet): what one enet_host_service() does with the datagrams of the room, when the
// acknowledgements go, how many are read at a stretch, when the socket is probed. Two real ENet hosts on the loopback, serviced
// by hand (no threads, no timers except a few short sleeps), so that every check is a count of datagrams, calls or events and
// not a measure of time. The socket calls of the platform layer are counted by the link (-Wl,--wrap=enet_socket_receive,
// enet_socket_send, enet_socket_wait: Tests/run.sh), which is also how a datagram is made to arrive "during a sendto".
//
// Build: Tests/run.sh (ENet with the PC's platform layer, third_party/enet/unix.c) and Tests/enet3ds/build.sh (the same
// test against the plugin's own layer, Sources/enet/enet_3ds.c, with AZL_E3DS defined).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <enet/enet.h>

#include "azl/room_client.hpp"
#include "azl/stats.hpp"
#include "support.hpp"

using namespace azl;

// ---- the platform layer's socket calls, counted -----------------------------------------------------------------------------
struct SocketCalls {
    u32 recv_data = 0;  ///< enet_socket_receive that returned a datagram
    u32 recv_empty = 0; ///< ... that found the socket empty (the probe that costs a soc call and returns nothing)
    u32 sends = 0;      ///< enet_socket_send (a datagram out)
    u32 waits = 0;      ///< enet_socket_wait (the poll)
    u32 recv_failed = 0; ///< enet_socket_receive made to fail (g_recv_fails)
};
static std::map<int, SocketCalls> g_calls;
/// ENet's clock: the real one, or - while a test holds it - a number of milliseconds that moves only when the test moves it, so that
/// the timers of the library (an acknowledgement's age, the room's retransmission timeout) are exact and no check is a race.
static bool g_vclock_on = false;
static enet_uint32 g_vclock = 0;
/// Called from the send wrapper, after the real send, for the socket in `g_after_send_socket`: a datagram that "arrives during the sendto".
static std::function<void()> g_after_send;
/// While true the wire is down: a datagram that reaches a socket is read and thrown away, the library finds the socket empty (TestSilenceProbe).
static bool g_hole = false;
/// While true the poll comes back at once with nothing readable (what the soc service does for a socket in trouble: the revents hold an error, not POLLIN)
/// and the receive fails: TestPollThatComesBackEarly.
static bool g_poll_early_none = false;
static bool g_recv_fails = false;
static int g_after_send_socket = -1;

extern "C" {
int __real_enet_socket_receive(ENetSocket, ENetAddress*, ENetBuffer*, size_t);
int __real_enet_socket_send(ENetSocket, const ENetAddress*, const ENetBuffer*, size_t);
int __real_enet_socket_wait(ENetSocket, enet_uint32*, enet_uint32);
enet_uint32 __real_enet_time_get(void);

enet_uint32 __wrap_enet_time_get(void) {
    return g_vclock_on ? g_vclock : __real_enet_time_get();
}

int __wrap_enet_socket_receive(ENetSocket s, ENetAddress* a, ENetBuffer* b, size_t n) {
    if (g_recv_fails) {
        ++g_calls[s].recv_failed;
        return -1;
    }
    int r = __real_enet_socket_receive(s, a, b, n);
    if (g_hole && r > 0) {
        r = 0;
    }
    if (r > 0) {
        ++g_calls[s].recv_data;
    } else if (r == 0) {
        ++g_calls[s].recv_empty;
    }
    return r;
}
int __wrap_enet_socket_send(ENetSocket s, const ENetAddress* a, const ENetBuffer* b, size_t n) {
    const int r = __real_enet_socket_send(s, a, b, n);
    ++g_calls[s].sends;
    if (s == g_after_send_socket && g_after_send) {
        auto f = std::move(g_after_send);
        g_after_send = nullptr;
        f();
    }
    return r;
}
int __wrap_enet_socket_wait(ENetSocket s, enet_uint32* c, enet_uint32 t) {
    ++g_calls[s].waits;
    if (g_poll_early_none) {
        *c = ENET_SOCKET_WAIT_NONE;
        return 0;
    }
    return __real_enet_socket_wait(s, c, t);
}
}

// ---- two hosts on the loopback ------------------------------------------------------------------------------------------------
static void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/// A server host (one peer) and a client connected to it, the way the plugin and the room are.
struct Pair {
    ENetHost* server = nullptr;
    ENetHost* client = nullptr;
    ENetPeer* sp = nullptr; ///< the server's end
    ENetPeer* cp = nullptr; ///< the client's end
    std::vector<std::string> got_server, got_client; ///< what each side was handed (packets, as text)

    /// `virtual_clock`: ENet's clock stands still until Advance() moves it (see g_vclock), from before the connection is made.
    explicit Pair(bool virtual_clock = false) {
        if (virtual_clock) {
            g_vclock = 1000000;
            g_vclock_on = true;
        }
        ENetAddress address{};
        address.host = ENET_HOST_ANY;
        address.port = 0;
        server = enet_host_create(&address, 1, 1, 0, 0);
        client = enet_host_create(nullptr, 1, 1, 0, 0);
        CHECK(server != nullptr && client != nullptr);
        if (!server || !client) {
            return;
        }
        ENetAddress to{};
        enet_address_set_host(&to, "127.0.0.1");
        to.port = server->address.port;
        cp = enet_host_connect(client, &to, 1, 0);
        sp = &server->peers[0];
        for (int i = 0; i < 400 && !(cp->state == ENET_PEER_STATE_CONNECTED && sp->state == ENET_PEER_STATE_CONNECTED); ++i) {
            Step(server, 1);
            Step(client, 1);
        }
        CHECK(cp->state == ENET_PEER_STATE_CONNECTED && sp->state == ENET_PEER_STATE_CONNECTED);
        // The plugin's settings (RoomClient::Run): the throttle never decelerates.
        enet_peer_throttle_configure(cp, ENET_PEER_PACKET_THROTTLE_INTERVAL, ENET_PEER_PACKET_THROTTLE_SCALE, 0);
        Settle();
    }
    ~Pair() {
        if (cp) {
            enet_peer_reset(cp);
        }
        if (client) {
            enet_host_destroy(client);
        }
        if (server) {
            enet_host_destroy(server);
        }
        g_vclock_on = false;
    }
    /// Moves ENet's clock (a Pair with the virtual clock).
    void Advance(enet_uint32 ms) {
        g_vclock += ms;
    }
    Pair(const Pair&) = delete;
    Pair& operator=(const Pair&) = delete;

    /// One service call of `host`; a packet it hands over is kept (as text) and destroyed. Returns what the call returned.
    int Step(ENetHost* host, u32 timeout_ms) {
        ENetEvent ev;
        const int r = enet_host_service(host, &ev, timeout_ms);
        if (r > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) {
            (host == server ? got_server : got_client).emplace_back(reinterpret_cast<const char*>(ev.packet->data), ev.packet->dataLength);
            enet_packet_destroy(ev.packet);
        }
        return r;
    }
    /// Service both hosts until a few calls in a row have nothing to do: the handshake's acknowledgements are out, nothing is in flight.
    void Settle() {
        for (int quiet = 0, i = 0; quiet < 4 && i < 200; ++i) {
            const u32 before = server->totalSentPackets + client->totalSentPackets;
            const int a = Step(server, 0), b = Step(client, 0);
            SleepMs(1);
            quiet = (a == 0 && b == 0 && server->totalSentPackets + client->totalSentPackets == before) ? quiet + 1 : 0;
        }
        got_server.clear();
        got_client.clear();
    }
    /// A reliable packet from the server, in a datagram of its own (the room flushes after every frame).
    void RoomSends(const std::string& text) {
        enet_peer_send(sp, 0, enet_packet_create(text.data(), text.size(), ENET_PACKET_FLAG_RELIABLE));
        enet_host_flush(server);
    }
    /// The client's service calls, until `n` packets were handed over or `max_calls` calls were made.
    void ClientUntil(std::size_t n, int max_calls = 200) {
        for (int i = 0; i < max_calls && got_client.size() < n; ++i) {
            Step(client, 0);
        }
    }
};

/// The first command of an ENet datagram as the host received it (host->receivedData): 1 = ACKNOWLEDGE.
static int FirstCommand(const ENetHost* host) {
    if (host->receivedDataLength < 3) {
        return -1;
    }
    const u16 peer_id = static_cast<u16>((host->receivedData[0] << 8) | host->receivedData[1]);
    const std::size_t head = (peer_id & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME) ? 4 : 2;
    return host->receivedDataLength > head ? (host->receivedData[head] & ENET_PROTOCOL_COMMAND_MASK) : -1;
}

// ---- counters (ENetAzlCounters) -----------------------------------------------------------------------------------------------

namespace {
bool g_drop_p2_once = false;
int InterceptDropP2(ENetHost* host, ENetEvent*) {
    const u8* d = host->receivedData;
    const std::size_t n = host->receivedDataLength;
    if (!g_drop_p2_once || n < 4) {
        return 0;
    }
    for (std::size_t i = 0; i + 3 < n; ++i) {
        if (d[i] == 0 && d[i + 1] == 2 && d[i + 2] == 'p' && d[i + 3] == '2') {
            g_drop_p2_once = false;
            return 1;
        }
    }
    return 0;
}
bool g_drop_first_ack = false;
int InterceptDropFirstAck(ENetHost* host, ENetEvent*) {
    if (g_drop_first_ack && FirstCommand(host) == ENET_PROTOCOL_COMMAND_ACKNOWLEDGE) {
        g_drop_first_ack = false;
        return 1;
    }
    return 0;
}
} // namespace

static void TestCounters() {
    std::printf("ENet counters: passes and the datagrams in them, acknowledgement datagrams, out of order, waits behind a hole, resent copies\n");
    // 1. A pass of five datagrams: counted as one pass of five; the acknowledgements of the five are one datagram of acks.
    {
        Pair p;
        ENetAzlCounters c;
        enet_azl_counters_take(p.client, &c); // (the handshake's: forgotten)
        for (int i = 1; i <= 5; ++i) {
            p.RoomSends("a" + std::to_string(i));
        }
        SleepMs(5);
        p.ClientUntil(5);
        p.Step(p.client, 0); // (the acknowledgements are out, whenever the library sends them)
        p.Step(p.client, 0);
        CHECK_EQ(p.got_client.size(), std::size_t(5));
        enet_azl_counters_take(p.client, &c);
        CHECK_EQ(c.passes, 1u);
        CHECK_EQ(c.passDatagramsMax, 5u);
        CHECK(c.passMsMax < 200);
        CHECK_EQ(c.ackOnlyDatagrams, 1u);
        CHECK_EQ(c.ackDataDatagrams, 0u);
        CHECK_EQ(c.oooCommands, 0u);
        CHECK_EQ(c.dupCommands, 0u);
        CHECK_EQ(c.holWaitMaxMs, 0u);
        // a take clears: a second one is empty
        enet_azl_counters_take(p.client, &c);
        CHECK_EQ(c.passes + c.passDatagramsMax + c.passMsMax + c.ackOnlyDatagrams + c.ackDataDatagrams, 0u);
    }
    // 2. An acknowledgement that rides on data: a datagram with an acknowledgement waiting and a packet queued goes out as one
    //    (the library's own queue of acknowledgements is filled by hand: the library sends them at once when it can).
    {
        Pair p;
        ENetAzlCounters c;
        enet_azl_counters_take(p.client, &c);
        ENetProtocol cmd;
        std::memset(&cmd, 0, sizeof(cmd));
        cmd.header.command = ENET_PROTOCOL_COMMAND_SEND_RELIABLE | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
        cmd.header.channelID = 0;
        cmd.header.reliableSequenceNumber = 9999; // (a sequence the server never sent: it ignores the acknowledgement)
        enet_peer_queue_acknowledgement(p.cp, &cmd, static_cast<enet_uint16>(p.client->serviceTime & 0xFFFF));
        const u32 sent = p.client->totalSentPackets;
        enet_peer_send(p.cp, 0, enet_packet_create("reply", 5, ENET_PACKET_FLAG_RELIABLE));
        enet_host_flush(p.client);
        CHECK_EQ(p.client->totalSentPackets - sent, 1u);
        enet_azl_counters_take(p.client, &c);
        CHECK_EQ(c.ackDataDatagrams, 1u);
        CHECK_EQ(c.ackOnlyDatagrams, 0u);
        // ... and one with only data is neither
        enet_peer_send(p.cp, 0, enet_packet_create("again", 5, ENET_PACKET_FLAG_RELIABLE));
        enet_host_flush(p.client);
        enet_azl_counters_take(p.client, &c);
        CHECK_EQ(c.ackDataDatagrams + c.ackOnlyDatagrams, 0u);
    }
    // 3. A datagram lost on the way: what comes behind it is out of order and waits; when the room resends it, all of them are
    //    handed over and the wait is what the hole cost.
    {
        Pair p(true);
        ENetAzlCounters c;
        enet_azl_counters_take(p.client, &c);
        p.client->intercept = InterceptDropP2;
        g_drop_p2_once = true;
        for (int i = 1; i <= 4; ++i) {
            p.RoomSends("p" + std::to_string(i));
            SleepMs(1);
        }
        SleepMs(5);
        p.ClientUntil(4, 20); // p1 only: p2 never arrived
        CHECK_EQ(p.got_client.size(), std::size_t(1));
        enet_azl_counters_take(p.client, &c);
        CHECK_EQ(c.oooCommands, 2u); // p3 and p4 came behind the hole
        CHECK_EQ(c.holWaitMaxMs, 0u); // (nothing handed over yet)
        p.Advance(60);
        // the server's retransmission timeout (a few ms on the loopback) has passed: servicing it resends p2
        for (int i = 0; i < 100 && p.got_client.size() < 4; ++i) {
            p.Step(p.server, 0);
            p.Step(p.client, 0);
            SleepMs(1);
        }
        CHECK_EQ(p.got_client.size(), std::size_t(4));
        CHECK(p.got_client.size() == 4 && p.got_client[1] == "p2" && p.got_client[3] == "p4");
        enet_azl_counters_take(p.client, &c);
        CHECK(c.holWaitMaxMs >= 40);   // p3 and p4 waited behind p2 for the best part of the 60 ms
        CHECK(c.holWaitMaxMs < 2000);
        CHECK_EQ(c.dupCommands, 0u);
        p.client->intercept = nullptr;
    }
    // 4. A copy the room resends because it did not get our acknowledgement: counted, and not handed over twice.
    {
        Pair p(true);
        ENetAzlCounters c;
        enet_azl_counters_take(p.client, &c);
        p.server->intercept = InterceptDropFirstAck;
        g_drop_first_ack = true;
        p.RoomSends("d1");
        SleepMs(2);
        p.ClientUntil(1);
        p.Step(p.client, 0); // (the acknowledgement is out)
        p.Step(p.client, 0);
        for (int i = 0; i < 4; ++i) {
            p.Step(p.server, 0); // the acknowledgement is swallowed here
            SleepMs(1);
        }
        CHECK(!g_drop_first_ack);
        p.Advance(60);
        for (int i = 0; i < 100 && p.client->azlCounters.dupCommands == 0; ++i) {
            p.Step(p.server, 0); // the retransmission timeout has passed: d1 goes again
            p.Step(p.client, 0);
            SleepMs(1);
        }
        CHECK_EQ(p.got_client.size(), std::size_t(1));
        enet_azl_counters_take(p.client, &c);
        CHECK_EQ(c.dupCommands, 1u);
        p.server->intercept = nullptr;
    }
    // 5. A reliable command whose sequence number is a window or more away from the one expected (a copy that comes long after the original): counted as
    //    a copy and not handed over; an unreliable command with the same number is not a copy of anything.
    {
        Pair p;
        ENetAzlCounters c;
        enet_azl_counters_take(p.client, &c);
        ENetProtocol cmd;
        std::memset(&cmd, 0, sizeof(cmd));
        cmd.header.command = ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE;
        cmd.header.channelID = 0;
        cmd.header.reliableSequenceNumber = static_cast<enet_uint16>(p.cp->channels[0].incomingReliableSequenceNumber - 0x3000);
        enet_peer_queue_incoming_command(p.cp, &cmd, "old", 3, 0, 0);
        enet_azl_counters_take(p.client, &c);
        CHECK_EQ(c.dupCommands, 0u);
        cmd.header.command = ENET_PROTOCOL_COMMAND_SEND_RELIABLE;
        enet_peer_queue_incoming_command(p.cp, &cmd, "old", 3, ENET_PACKET_FLAG_RELIABLE, 0);
        enet_azl_counters_take(p.client, &c);
        CHECK_EQ(c.dupCommands, 1u);
        CHECK_EQ(p.Step(p.client, 0), 0); // (nothing was queued for the caller)
        CHECK_EQ(p.got_client.size(), std::size_t(0));
    }
}

// ---- the receive that only says "nothing" ---------------------------------------------------------------------------------------
// A service call that is going to poll does not first ask the socket whether it has something when it was found empty a moment ago:
// the poll says so at once. Before, the probe was made after every acknowledgement (a send cleared the plugin's guess), two empty
// answers for each datagram read; and a datagram that came during that send must still be read no later than one service call.
static void TestEmptyProbe() {
    std::printf("ENet: no receive that only answers \"nothing\" when a poll follows; a datagram that comes during a send is read by the next call at the latest\n");
    // 1. One datagram, then a call that only waits (the plugin's loop: the next round polls for 1 to 3 ms).
    {
        Pair p;
        SocketCalls& k = g_calls[p.client->socket];
        p.RoomSends("one");
        SleepMs(2);
        k = SocketCalls();
        p.Step(p.client, 50); // the pass reads the datagram and the socket is empty after it
        CHECK_EQ(p.got_client.size(), std::size_t(1));
        CHECK_EQ(k.recv_data, 1u);
        CHECK_EQ(k.recv_empty, 1u); // the one that ends the pass
        const u32 empty_a = k.recv_empty, waits_a = k.waits;
        p.Step(p.client, 10); // nothing to read, nothing to send: the call polls
        CHECK_EQ(k.waits - waits_a, 1u);
        CHECK_EQ(k.recv_empty - empty_a, 0u); // ... and does not probe first (before: a second empty answer)
        p.Step(p.client, 10); // and again, twice: a poll that timed out is remembered as well as a receive that found nothing
        p.Step(p.client, 10);
        CHECK_EQ(k.waits - waits_a, 3u);
        CHECK_EQ(k.recv_empty - empty_a, 0u);
        // a call with no time to wait must look: the datagram that came meanwhile is handed over by it
        p.RoomSends("two");
        SleepMs(2);
        p.Step(p.client, 0);
        CHECK_EQ(p.got_client.size(), std::size_t(2));
    }
    // 2. Twenty datagrams that come while the client waits in its poll, 8 ms apart (the room's frames: the thread of the network loop
    //    sleeps in poll and is woken by each): one empty answer per datagram, not two.
    {
        Pair p;
        SocketCalls& k = g_calls[p.client->socket];
        p.Step(p.client, 0);
        k = SocketCalls();
        std::thread sender([&p] {
            for (int i = 0; i < 20; ++i) {
                SleepMs(8);
                p.RoomSends("n" + std::to_string(i));
            }
        });
        for (int calls = 0; calls < 400 && p.got_client.size() < 20; ++calls) {
            p.Step(p.client, 20);
        }
        sender.join();
        CHECK_EQ(p.got_client.size(), std::size_t(20));
        CHECK_EQ(k.recv_data, 20u);
        std::printf("  20 datagrams: %u receives with data, %u empty, %u sends, %u polls\n", k.recv_data, k.recv_empty, k.sends, k.waits);
        CHECK(k.recv_empty <= 22); // (before: 40)
        CHECK(k.waits <= 26);      // (the poll that wakes is the same one, before and after)
    }
    // 3. A datagram comes while the client is inside a send: it is handed over by the call after the one that sent, whatever the
    //    library does in between.
    {
        Pair p;
        p.RoomSends("first");
        SleepMs(2);
        g_after_send_socket = p.client->socket;
        g_after_send = [&p] {
            p.RoomSends("late");
            SleepMs(1);
        };
        p.Step(p.client, 30); // "first"; its acknowledgement goes out in this call or in the next, and "late" arrives during that send
        CHECK_EQ(p.got_client.size(), std::size_t(1));
        p.Step(p.client, 30);
        if (p.got_client.size() < 2) {
            p.Step(p.client, 30);
        }
        CHECK_EQ(p.got_client.size(), std::size_t(2));
        CHECK(p.got_client.size() == 2 && p.got_client[1] == "late");
        g_after_send_socket = -1;
    }
    // 4. The same, with a send so slow that the time given to the call runs out in it: the call returns without having polled, and
    //    the next one must read.
    {
        Pair p;
        p.RoomSends("x");
        SleepMs(2);
        p.Step(p.client, 50);
        CHECK_EQ(p.got_client.size(), std::size_t(1));
        p.Step(p.client, 5);  // (nothing: the socket is known to be empty now)
        enet_peer_send(p.cp, 0, enet_packet_create("y", 1, ENET_PACKET_FLAG_RELIABLE));
        g_after_send_socket = p.client->socket;
        g_after_send = [&p] {
            p.RoomSends("late");
            SleepMs(8); // (the sendto takes longer than the 1 ms the call was given)
        };
        p.Step(p.client, 1);
        if (p.got_client.size() < 2) {
            p.Step(p.client, 1);
        }
        CHECK_EQ(p.got_client.size(), std::size_t(2));
        g_after_send_socket = -1;
    }
    // 4b. The skip is for ONE call: a call that left the receive out and ran out of time in its sends without polling forgets that the socket
    //     was empty. With a send that is slow every time (a busy soc service) the receive would otherwise never be made: both calls have
    //     1 ms and a send of 8 ms, and the datagram that came during the first send is handed over by the second at the latest.
    {
        Pair p;
        p.RoomSends("x");
        SleepMs(2);
        p.Step(p.client, 50);
        CHECK_EQ(p.got_client.size(), std::size_t(1));
        p.Step(p.client, 5); // (the socket is known to be empty)
        enet_peer_send(p.cp, 0, enet_packet_create("y", 1, ENET_PACKET_FLAG_UNSEQUENCED)); // (nothing to be acknowledged: no timer of ours runs, which would stop the skip)
        g_after_send_socket = p.client->socket;
        g_after_send = [&p] {
            p.RoomSends("late");
            SleepMs(8);
        };
        p.Step(p.client, 1); // no receive (the socket was empty), "y" goes out slowly, the time is gone: the call returns without polling
        enet_peer_send(p.cp, 0, enet_packet_create("z", 1, ENET_PACKET_FLAG_UNSEQUENCED));
        g_after_send = [] { SleepMs(8); };
        p.Step(p.client, 1); // its send is slow too: it must read the socket first, not skip again
        CHECK_EQ(p.got_client.size(), std::size_t(2)); // (a skip that is not forgotten leaves "late" in the socket for as long as the sends are slow)
        g_after_send_socket = -1;
        g_after_send = nullptr;
    }
    // 5. The socket is known to be empty and the timer of a packet of ours has run out meanwhile (the thread was away, a failing socket came
    //    back): the acknowledgement that stops the timer is in the socket by now. The receive is NOT left out then - the check of the timers
    //    comes next, and a packet whose timer is past 5 s is given up with its peer (the model's HoR, a failing socket of 6 s: lost at once).
    {
        Pair p;
        p.cp->roundTripTime = 20; // (the timer of a packet of ours: 20 ms)
        p.cp->roundTripTimeVariance = 0;
        const u32 lost0 = p.cp->packetsLost;
        p.Step(p.client, 5); // the poll times out: the socket is known to be empty
        enet_peer_send(p.cp, 0, enet_packet_create("u", 1, ENET_PACKET_FLAG_RELIABLE));
        enet_host_flush(p.client);
        p.Step(p.server, 0); // the room reads it ...
        p.Step(p.server, 0); // ... and acknowledges it: the acknowledgement waits in our socket
        SleepMs(60);         // the thread was away: the timer has run out
        p.Step(p.client, 5);
        CHECK_EQ(p.cp->packetsLost - lost0, 0u); // (before: the receive was left out, the timers were checked first, and "u" went out again)
        CHECK(enet_list_empty(&p.cp->sentReliableCommands));
    }
}

// A poll that comes back before its time with nothing readable says nothing about the socket. The soc service reports a socket in trouble that way
// (the revents of the poll hold POLLERR, not POLLIN): the library must not take the socket for empty and leave the receive out - the receive is where the
// error is reported. (The first version of the series did, and a socket in trouble made the calls come back at once, one poll after another, until a
// timer of ours ran out.)
static void TestPollThatComesBackEarly() {
    std::printf("ENet: a poll that comes back early with nothing readable does not make the socket \"known empty\": the next call reads it and sees the error\n");
    Pair p;
    SocketCalls& k = g_calls[p.client->socket];
    p.Step(p.client, 5); // the poll times out the whole time: the socket is known to be empty (the receive of the next call is left out)
    const u32 failed0 = k.recv_failed;
    g_poll_early_none = true;
    g_recv_fails = true;
    int first_error_call = 0;
    for (int call = 1; call <= 6 && first_error_call == 0; ++call) {
        if (p.Step(p.client, 20) < 0) {
            first_error_call = call;
        }
    }
    g_poll_early_none = false;
    g_recv_fails = false;
    // The call that finds the socket "known empty" leaves the receive out and polls (the poll comes back at once, nothing readable); from then on the
    // socket is not known to be empty, so the next call reads and the failing receive is seen: at the second call. (Before the guard: never.)
    CHECK(first_error_call >= 1 && first_error_call <= 2);
    CHECK(k.recv_failed - failed0 >= 1u);
    // And a poll that does wait the whole time still counts: the receive of the calls after it is left out (the guard must not take the skip away).
    {
        p.Step(p.client, 10); // (the socket is not known to be empty: this call reads, finds nothing and polls the whole 10 ms)
        const u32 empty_a = k.recv_empty, waits_a = k.waits;
        p.Step(p.client, 10); // known empty: no receive, the real poll again
        p.Step(p.client, 10);
        CHECK_EQ(k.waits - waits_a, 2u);
        CHECK_EQ(k.recv_empty - empty_a, 0u);
    }
}

// ---- the backlog ----------------------------------------------------------------------------------------------------------------

// A backlog in the socket (the thread was away for 100 ms, or the game had the CPU) is read in pieces: a service call stops after 32
// datagrams, hands over the first packet and the next call reads on, so that the first frame of a burst
// of 100 is not held until the last is read (150 ms at 1.5 ms a datagram on the console).
static void TestReadInPieces() {
    std::printf("ENet: a backlog is read in pieces (32 datagrams), the first packet is handed over after the first piece, nothing is lost or reordered\n");
    constexpr int kBurst = 100;
    // 1. The count limit.
    {
        Pair p;
        for (int i = 0; i < kBurst; ++i) {
            p.RoomSends("b" + std::to_string(i));
        }
        SleepMs(10);
        const u32 before = p.client->totalReceivedPackets;
        CHECK_EQ(p.Step(p.client, 0), 1); // the first packet is there
        const u32 read = p.client->totalReceivedPackets - before;
        std::printf("  first call: %u datagrams read of %d\n", read, kBurst);
        CHECK(read >= 1 && read <= 32); // (before: all 100)
        CHECK_EQ(p.got_client.size(), std::size_t(1));
        CHECK(p.got_client.size() == 1 && p.got_client[0] == "b0");
        int calls = 1;
        for (; calls < 1000 && p.got_client.size() < kBurst; ++calls) {
            p.Step(p.client, 0);
        }
        CHECK_EQ(p.got_client.size(), std::size_t(kBurst));
        bool in_order = p.got_client.size() == kBurst;
        for (int i = 0; in_order && i < kBurst; ++i) {
            in_order = p.got_client[i] == "b" + std::to_string(i);
        }
        CHECK(in_order);
        // everything was acknowledged: the room has nothing in flight
        for (int i = 0; i < 6; ++i) {
            p.Step(p.client, 0);
            p.Step(p.server, 0);
            SleepMs(1);
        }
        CHECK_EQ(p.sp->reliableDataInTransit, 0u);
        CHECK(enet_list_empty(&p.sp->sentReliableCommands));
    }
    // 2. Calls that are willing to wait do not poll between the pieces (the socket holds more): the server below reads 100 datagrams
    //    in calls of 50 ms and polls (at most) once, when it has caught up.
    {
        Pair p;
        for (int i = 0; i < kBurst; ++i) {
            const std::string text = "c" + std::to_string(i);
            enet_peer_send(p.cp, 0, enet_packet_create(text.data(), text.size(), ENET_PACKET_FLAG_RELIABLE));
            enet_host_flush(p.client);
        }
        SleepMs(10);
        SocketCalls& k = g_calls[p.server->socket];
        k = SocketCalls();
        for (int calls = 0; calls < 1000 && p.got_server.size() < kBurst; ++calls) {
            p.Step(p.server, 50);
        }
        CHECK_EQ(p.got_server.size(), std::size_t(kBurst));
        std::printf("  server: %u datagrams read in %u polls\n", k.recv_data, k.waits);
        CHECK(k.waits <= 2);
    }
}

// The limit is a field of the host, 0 = ENet's own (256 datagrams).
static void TestReadKnobs() {
    std::printf("ENet: the read limit can be switched off per host\n");
    constexpr int kBurst = 100;
    Pair p;
    p.client->azlMaxDatagrams = 0;
    for (int i = 0; i < kBurst; ++i) {
        p.RoomSends("o" + std::to_string(i));
    }
    SleepMs(10);
    const u32 before = p.client->totalReceivedPackets;
    CHECK_EQ(p.Step(p.client, 0), 1);
    CHECK_EQ(p.client->totalReceivedPackets - before, u32(kBurst));
    CHECK_EQ(p.client->azlSkipEmptyRecv, 1u); // the other defaults of the plugin's host
    ENetHost* fresh = enet_host_create(nullptr, 1, 1, 0, 0);
    CHECK(fresh && fresh->azlMaxDatagrams == 32);
    if (fresh) {
        enet_host_destroy(fresh);
    }
}

// The mark "the socket is empty" belongs to ONE look at the socket: a pass that starts clears it. A pass the cap ends (the socket still holds datagrams) must not leave the
// mark of an earlier poll behind, or the next call leaves out the receive of a socket that is not empty and polls for what is there. The pass here is started by a timer that
// is due (so the skip does not apply); the acknowledgement that stops the timer is the first of its datagrams.
static void TestEmptyMarkIsClearedByAPass() {
    std::printf("ENet: a pass cut short by the cap does not leave the mark \"the socket is empty\" behind\n");
    Pair p;
    p.cp->roundTripTime = 20; // (the timer of a packet of ours: 20 ms)
    p.cp->roundTripTimeVariance = 0;
    p.client->azlMaxDatagrams = 2;
    SocketCalls& k = g_calls[p.client->socket];
    p.Step(p.client, 5); // the poll times out: the socket is known to be empty
    enet_peer_send(p.cp, 0, enet_packet_create("u", 1, ENET_PACKET_FLAG_RELIABLE));
    enet_host_flush(p.client);
    p.Step(p.server, 0); // the room reads it ...
    p.Step(p.server, 0); // ... and acknowledges it: the acknowledgement is the first datagram in our socket
    for (int i = 0; i < 5; ++i) {
        p.RoomSends("d" + std::to_string(i)); // ... and five more behind it
    }
    SleepMs(60); // the timer of "u" has run out: the next call reads before it checks
    CHECK_EQ(p.Step(p.client, 5), 1); // the pass: the acknowledgement and "d0", then the cap
    CHECK(enet_list_empty(&p.cp->sentReliableCommands)); // (no timer of ours is due now)
    const u32 waits_a = k.waits;
    CHECK_EQ(p.Step(p.client, 5), 1); // the socket is not empty: this call reads it (before: it skipped the receive, polled, and read after the poll)
    CHECK_EQ(k.waits - waits_a, 0u);
    CHECK_EQ(p.got_client.size(), std::size_t(2));
}

// The acknowledgement of a packet of ours waits in the socket behind more datagrams than a pass reads (the thread was away, the timer of the packet
// has run out meanwhile). ENet's call reads the socket to its end and only then checks the timers (AzaharLink: the first pass of a call does not
// check them at all); a pass that stops at the limit must not check them either, or the packet goes out again for nothing and its timer doubles -
// and a peer whose oldest packet is past 5 s is given up although the acknowledgement is there (the model's HoR, a failing socket of 6 s).
static void TestAckBehindBacklog() {
    std::printf("ENet: an acknowledgement that waits in the socket behind more datagrams than a pass reads is not a timeout\n");
    Pair p(true);
    p.client->azlAckHoldEvents = 0; // (ENet's order: the call goes on from the first pass to the check of the timers; with the hold the packets of the pass are handed over first)
    p.cp->roundTripTime = 20; // (the timer of a packet of ours: 20 ms)
    p.cp->roundTripTimeVariance = 0;
    const u32 lost0 = p.cp->packetsLost;
    for (int i = 0; i < 40; ++i) {
        p.RoomSends("f" + std::to_string(i)); // 40 datagrams of the room ...
    }
    enet_peer_send(p.cp, 0, enet_packet_create("u", 1, ENET_PACKET_FLAG_RELIABLE));
    enet_host_flush(p.client);
    p.Step(p.server, 0); // ... then the room reads "u" (hands it over) ...
    p.Step(p.server, 0); // ... and acknowledges it: the 41st datagram in our socket
    p.Advance(100);      // the thread was away: the 20 ms of "u" have run out
    for (int i = 0; i < 60 && p.got_client.size() < 40; ++i) {
        p.Step(p.client, 0);
    }
    for (int i = 0; i < 4; ++i) {
        p.Step(p.client, 0);
    }
    CHECK_EQ(p.got_client.size(), std::size_t(40));
    CHECK_EQ(p.cp->packetsLost - lost0, 0u); // (before: the first call read 32 datagrams, found "u" late and sent it again)
    CHECK(enet_list_empty(&p.cp->sentReliableCommands));
}

// ---- the packet first, the acknowledgement after ------------------------------------------------------------------------------

// ENet sent the acknowledgements of the datagrams it had read before it handed over the packets in them, a send to the soc service
// (0.3 to 4 ms on the console) in front of every frame. Now the packet goes first and the acknowledgement follows at the start of the next
// call, in the same datagram as whatever the caller queued meanwhile (the reply to the frame); after 8 packets, or 8 ms, it goes first.
static void TestAckAfterEvent() {
    std::printf("ENet: the packet is handed over before the acknowledgement is sent; the acknowledgement rides on the reply; it waits for 8 packets or 8 ms at the most\n");
    // 1. One packet: handed over with no datagram sent; the next call sends the acknowledgement.
    {
        Pair p;
        p.RoomSends("e1");
        SleepMs(2);
        const u32 sent0 = p.client->totalSentPackets;
        CHECK_EQ(p.Step(p.client, 0), 1);
        CHECK_EQ(p.got_client.size(), std::size_t(1));
        CHECK_EQ(p.client->totalSentPackets, sent0); // (before: the acknowledgement was out already)
        p.Step(p.client, 0);
        CHECK_EQ(p.client->totalSentPackets - sent0, 1u);
        for (int i = 0; i < 4; ++i) { // the room got it: nothing in flight
            p.Step(p.server, 0);
            SleepMs(1);
        }
        CHECK_EQ(p.sp->reliableDataInTransit, 0u);
        CHECK(enet_list_empty(&p.sp->sentReliableCommands));
    }
    // 2. The reply the caller queues after the packet goes in the same datagram as the acknowledgement: one datagram, not two.
    {
        Pair p;
        p.RoomSends("q");
        SleepMs(2);
        const u32 sent0 = p.client->totalSentPackets;
        CHECK_EQ(p.Step(p.client, 0), 1);
        enet_peer_send(p.cp, 0, enet_packet_create("reply", 5, ENET_PACKET_FLAG_RELIABLE)); // (RoomClient::Run: the frames of the game, then a flush)
        enet_host_flush(p.client);
        CHECK_EQ(p.client->totalSentPackets - sent0, 1u); // (before: 2, the acknowledgement alone and then the reply)
        for (int i = 0; i < 6 && p.got_server.empty(); ++i) {
            p.Step(p.server, 0);
            SleepMs(1);
        }
        CHECK_EQ(p.got_server.size(), std::size_t(1));
        CHECK(p.got_server.size() == 1 && p.got_server[0] == "reply");
        for (int i = 0; i < 4; ++i) {
            p.Step(p.server, 0);
            SleepMs(1);
        }
        CHECK_EQ(p.sp->reliableDataInTransit, 0u); // (and "q" was acknowledged by the same datagram)
    }
    // 3. Twenty packets in one pass: the first eight are handed over with no acknowledgement sent, the ninth call sends them
    //    (one datagram for the eight), and so on.
    {
        Pair p(true);
        for (int i = 0; i < 20; ++i) {
            p.RoomSends("k" + std::to_string(i));
        }
        SleepMs(5);
        const u32 sent0 = p.client->totalSentPackets;
        int first_ack_after = -1;
        for (int calls = 0; calls < 40 && p.got_client.size() < 20; ++calls) {
            const std::size_t before = p.got_client.size();
            const u32 sent_before = p.client->totalSentPackets;
            p.Step(p.client, 0);
            if (first_ack_after < 0 && p.client->totalSentPackets != sent_before) {
                first_ack_after = static_cast<int>(before);
            }
        }
        CHECK_EQ(p.got_client.size(), std::size_t(20));
        std::printf("  20 packets: the first acknowledgement went out after %d of them; %u datagrams in all\n", first_ack_after,
                    p.client->totalSentPackets - sent0);
        CHECK_EQ(first_ack_after, 8); // (before: 0, the acknowledgement in front of the first packet)
    }
    // 3b. (verification, filone P5a) The count of packets starts again once the acknowledgements went out: a SECOND burst of datagrams
    //     read after the first batch of acknowledgements was sent is held for 8 packets again, not for none.
    {
        Pair p(true);
        for (int i = 0; i < 20; ++i) {
            p.RoomSends("a" + std::to_string(i));
        }
        SleepMs(5);
        for (int calls = 0; calls < 40 && p.got_client.size() < 20; ++calls) {
            p.Step(p.client, 0);
        }
        p.Step(p.client, 0); // (nothing waits any more)
        CHECK_EQ(p.got_client.size(), std::size_t(20));
        CHECK_EQ(p.client->azlAckSince, 0u);
        for (int i = 0; i < 12; ++i) {
            p.RoomSends("b" + std::to_string(i));
        }
        SleepMs(5);
        int second_first_ack_after = -1;
        for (int calls = 0; calls < 40 && p.got_client.size() < 32; ++calls) {
            const std::size_t before = p.got_client.size() - 20;
            const u32 sent_before = p.client->totalSentPackets;
            p.Step(p.client, 0);
            if (second_first_ack_after < 0 && p.client->totalSentPackets != sent_before) {
                second_first_ack_after = static_cast<int>(before);
            }
        }
        CHECK_EQ(p.got_client.size(), std::size_t(32));
        std::printf("  second burst: the first acknowledgement went out after %d of 12 packets\n", second_first_ack_after);
        CHECK_EQ(second_first_ack_after, 8);
    }
    // 4. A packet that waits in the caller for longer than the hold (8 ms at the most, less on a link that answers faster): its
    //    acknowledgement goes out before the next packet is handed over.
    {
        Pair p(true);
        p.RoomSends("t1");
        p.RoomSends("t2");
        SleepMs(2);
        const u32 sent0 = p.client->totalSentPackets;
        CHECK_EQ(p.Step(p.client, 0), 1);
        CHECK_EQ(p.client->totalSentPackets, sent0);
        p.Advance(12); // (the caller is busy with the first packet)
        CHECK_EQ(p.Step(p.client, 0), 1);
        CHECK_EQ(p.got_client.size(), std::size_t(2));
        CHECK_EQ(p.client->totalSentPackets - sent0, 1u); // the acknowledgement went out in this call, ahead of the second packet
    }
    // 5. On a link that answers faster than a WiFi one the hold is a third of the room's timeout (what we can tell of it: rtt + 4 variance),
    //    never less than 2 ms: with an estimate of 6 ms the acknowledgement of a packet that waited 3 ms goes out before the next one.
    {
        Pair p(true);
        p.cp->roundTripTime = 6;
        p.cp->roundTripTimeVariance = 0;
        p.RoomSends("f1");
        p.RoomSends("f2");
        SleepMs(2);
        const u32 sent0 = p.client->totalSentPackets;
        CHECK_EQ(p.Step(p.client, 0), 1);
        CHECK_EQ(p.client->totalSentPackets, sent0);
        p.Advance(3); // (a third of 6 ms is 2: gone; the 8 ms of a WiFi link are not)
        CHECK_EQ(p.Step(p.client, 0), 1);
        CHECK_EQ(p.client->totalSentPackets - sent0, 1u);
    }
    // 5b. ... and the floor: with an estimate of 1 ms the hold is 2 ms, not none: a packet that waited 1 ms leaves the acknowledgement
    //     where it is, one that waited 6 ms does not.
    {
        Pair p(true);
        p.cp->roundTripTime = 1;
        p.cp->roundTripTimeVariance = 0;
        p.RoomSends("g1");
        p.RoomSends("g2");
        p.RoomSends("g3");
        SleepMs(2);
        const u32 sent0 = p.client->totalSentPackets;
        CHECK_EQ(p.Step(p.client, 0), 1);
        p.Advance(1);
        CHECK_EQ(p.Step(p.client, 0), 1);
        CHECK_EQ(p.client->totalSentPackets, sent0); // (1 ms is under the floor of 2: still held)
        p.Advance(5);
        CHECK_EQ(p.Step(p.client, 0), 1);
        CHECK_EQ(p.client->totalSentPackets - sent0, 1u);
        CHECK_EQ(p.got_client.size(), std::size_t(3));
    }
}

// The age of the acknowledgements that wait is the age of the OLDEST one: queueing a second acknowledgement later does not move the stamp (white box: the queue function
// is called by hand with the host's clock moved in between; in the flow of the library every acknowledgement of a pass is queued on the same clock reading and the send
// phase clears the stamp before the next pass, so the flow cannot tell the first from the last - the contract is the function's).
static void TestAckStampIsKept() {
    std::printf("ENet: the stamp of the oldest acknowledgement that waits is kept when a later one is queued\n");
    Pair p(true);
    ENetProtocol cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.header.command = ENET_PROTOCOL_COMMAND_SEND_RELIABLE | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
    cmd.header.channelID = 0;
    cmd.header.reliableSequenceNumber = 9999; // (a sequence the server never sent: it ignores the acknowledgement)
    CHECK_EQ(p.client->azlAckSince, 0u);
    p.client->serviceTime = 5000;
    enet_peer_queue_acknowledgement(p.cp, &cmd, 0);
    CHECK_EQ(p.client->azlAckSince, 5000u);
    p.client->serviceTime = 5007;
    enet_peer_queue_acknowledgement(p.cp, &cmd, 0);
    CHECK_EQ(p.client->azlAckSince, 5000u); // (the oldest one counts)
    enet_host_flush(p.client);
    CHECK_EQ(p.client->azlAckSince, 0u); // (and the send clears it)
}

// What keeps the hold from costing a retransmission (uses the counters and the knobs: it does not exist for the original library).
static void TestAckHoldLimit() {
    std::printf("ENet: a slow caller and a burst: the hold limit keeps the room from resending\n");
    // Slow caller, burst, a room that retransmits after 28 ms: nothing is resent, because no acknowledgement waits long. With no
    // limit on the wait the same burst is resent (the limit is what keeps it from happening).
    for (int limited = 1; limited >= 0; --limited) {
        Pair p(true);
        if (!limited) {
            p.client->azlAckHoldEvents = 1000;
            p.client->azlAckHoldMs = 0;
        }
        p.sp->roundTripTime = 20;
        p.sp->roundTripTimeVariance = 2; // (the room's timeout for a packet: rtt + 4 var = 28 ms)
        ENetAzlCounters c;
        enet_azl_counters_take(p.client, &c);
        const u32 lost0 = p.sp->packetsLost;
        for (int i = 0; i < 30; ++i) {
            p.RoomSends("s" + std::to_string(i));
        }
        for (int calls = 0; calls < 400 && p.got_client.size() < 30; ++calls) {
            p.Step(p.server, 0);
            if (p.Step(p.client, 0) == 1) {
                p.Advance(3); // (the caller's work on each packet: a frame through the engine with the log on)
            }
        }
        for (int i = 0; i < 20; ++i) {
            p.Step(p.client, 0);
            p.Step(p.server, 0);
            p.Advance(1);
        }
        enet_azl_counters_take(p.client, &c);
        std::printf("  %s: %zu packets handed over, the room resent %u, copies received %u, longest ack hold %u ms\n",
                    limited ? "limited (8 packets, 8 ms)" : "no limit", p.got_client.size(), p.sp->packetsLost - lost0, c.dupCommands,
                    c.ackHoldMaxMs);
        CHECK_EQ(p.got_client.size(), std::size_t(30));
        if (limited) {
            CHECK_EQ(p.sp->packetsLost - lost0, 0u);
            CHECK_EQ(c.dupCommands, 0u);
            CHECK(c.ackHoldMaxMs <= 12);
        } else {
            CHECK(p.sp->packetsLost - lost0 > 0); // (the control: without the limit the burst IS resent)
        }
    }
}

// The behaviour can be switched off per host: ENet's order, the acknowledgement in front of the packet.
static void TestAckOrderKnob() {
    std::printf("ENet: azlAckHoldEvents = 0 is ENet's order (acknowledgement first)\n");
    Pair p;
    p.client->azlAckHoldEvents = 0;
    p.RoomSends("e1");
    SleepMs(2);
    const u32 sent0 = p.client->totalSentPackets;
    CHECK_EQ(p.Step(p.client, 0), 1);
    CHECK_EQ(p.client->totalSentPackets - sent0, 1u);
    ENetHost* fresh = enet_host_create(nullptr, 1, 1, 0, 0);
    CHECK(fresh && fresh->azlAckHoldEvents == 8 && fresh->azlAckHoldMs == 8);
    if (fresh) {
        enet_host_destroy(fresh);
    }
}

// ---- the room link after a silence --------------------------------------------------------------------------------------------
// The WiFi goes away for a few seconds and comes back while the console has a packet the room has not answered. ENet doubles that packet's timer at
// every try and gives the peer up at the first check that finds it 5 s old with six tries behind it: with a round trip of 26 ms the tries are at
// 26, 78, 182, 390, 806, 1638, 3302 and 6630 ms, so a link that is back at 3.5 s is still declared lost at 6.63 s - at the check, before the try that
// the link would have answered. ShouldProbeRoom (RoomClient::Run) says when to ping the room instead: when it speaks again after half a second of
// silence; the answer restarts the count. Here both hosts have the same virtual clock, the datagrams are read and dropped by the socket wrapper while the hole lasts (g_hole), and
// the loop is the client's: a service call, then the probe's question, as RoomClient::Run asks it.
namespace {
struct Outage {
    bool lost = false;      ///< the client was handed a DISCONNECT
    u32 lost_at = 0;        ///< ... this many ms after the packet nobody answered
    u32 probes = 0;         ///< pings the probe sent
    std::string probe_times; ///< ... and when (ms after the packet nobody answered, with the age of the oldest unanswered one then)
    bool still_ok = false;  ///< both ends were connected at the end
};
} // namespace

static Outage RunOutage(u32 outage_ms, bool probe_on, u32 horizon_ms) {
    Pair p(true);
    p.cp->roundTripTime = 26; // (the round trip of a console on a LAN with the room; the timers start from it)
    p.cp->roundTripTimeVariance = 0;
    p.sp->roundTripTime = 26;
    p.sp->roundTripTimeVariance = 0;
    g_hole = true;
    enet_peer_send(p.cp, 0, enet_packet_create("u", 1, ENET_PACKET_FLAG_RELIABLE)); // the packet nobody answers
    enet_host_flush(p.client);
    Outage o;
    u32 seen = p.client->totalReceivedPackets;
    u64 last_probe = 0;
    for (u32 t = 0; t < horizon_ms; ++t) {
        if (t == outage_ms) {
            g_hole = false;
        }
        p.Advance(1);
        p.Step(p.server, 0);
        ENetEvent ev;
        const int r = enet_host_service(p.client, &ev, 0);
        if (r > 0 && ev.type == ENET_EVENT_TYPE_DISCONNECT) {
            o.lost = true;
            o.lost_at = t;
            break;
        }
        if (r > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) {
            enet_packet_destroy(ev.packet);
        }
        if (probe_on && p.client->totalReceivedPackets != seen) {
            seen = p.client->totalReceivedPackets;
            if (ShouldProbeRoom(p.client->serviceTime, p.cp->earliestTimeout, g_vclock, last_probe)) {
                o.probe_times += " " + std::to_string(t) + "(age " + std::to_string(p.client->serviceTime - p.cp->earliestTimeout) + ")";
                enet_peer_ping(p.cp);
                last_probe = g_vclock;
                ++o.probes;
            }
        }
    }
    o.still_ok = !o.lost && p.cp->state == ENET_PEER_STATE_CONNECTED && p.sp->state == ENET_PEER_STATE_CONNECTED;
    g_hole = false;
    return o;
}

static void TestSilenceProbe() {
    std::printf("ENet: the WiFi is away for 3.5 s and back: without the probe the room is declared lost at 6.6 s, with it the link is kept; a longer silence is not made worse\n");
    // the question itself
    CHECK(!ShouldProbeRoom(10000, 0, 5000, 0));                  // nothing of ours is waiting for an answer
    CHECK(!ShouldProbeRoom(10000, 9600, 5000, 0));               // waiting 400 ms: that is a timer, not a silence
    CHECK(ShouldProbeRoom(10000, 9500, 5000, 0));                // waiting 500 ms
    CHECK(!ShouldProbeRoom(10000, 9500, 5000, 4800));            // pinged 200 ms ago
    CHECK(ShouldProbeRoom(10000, 9500, 5000, 4750));             // pinged 250 ms ago
    CHECK(ShouldProbeRoom(600, 4294967000u, 5000, 0));           // (the host's clock wraps: the age is a difference)
    // 3.5 s
    const Outage off = RunOutage(3500, false, 15000);
    const Outage on = RunOutage(3500, true, 15000);
    std::printf("  3.5 s, no probe: %s at %u ms; probe: %s, %u ping(s) at%s\n", off.lost ? "declared lost" : "kept", off.lost_at, on.lost ? "declared lost" : "kept", on.probes,
                on.probe_times.c_str());
    CHECK(off.lost && off.lost_at >= 6000 && off.lost_at <= 7200); // (the hazard: the check at 6.63 s)
    CHECK(on.still_ok);
    CHECK(on.probes >= 1 && on.probes <= 3);
    // 1 s: nothing to lose either way
    const Outage short_off = RunOutage(1000, false, 12000);
    const Outage short_on = RunOutage(1000, true, 12000);
    CHECK(short_off.still_ok && short_on.still_ok);
    // 5.5 s: the room has given up on the console as well, the probe does not make the end later
    const Outage long_off = RunOutage(5500, false, 20000);
    const Outage long_on = RunOutage(5500, true, 20000);
    std::printf("  5.5 s, no probe: lost at %u ms; probe: %s at %u ms\n", long_off.lost_at, long_on.lost ? "lost" : "kept", long_on.lost_at);
    CHECK(long_off.lost && long_on.lost);
    CHECK(long_on.lost_at <= long_off.lost_at + 100);
}

int main() {
    CHECK_EQ(enet_initialize(), 0);
    TestCounters();
    TestAckAfterEvent();
    TestAckHoldLimit();
    TestAckOrderKnob();
    TestAckStampIsKept();
    TestEmptyProbe();
    TestPollThatComesBackEarly();
    TestReadInPieces();
    TestReadKnobs();
    TestEmptyMarkIsClearedByAPass();
    TestAckBehindBacklog();
    TestSilenceProbe();
    enet_deinitialize();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
