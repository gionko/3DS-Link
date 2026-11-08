// chat_room_probe: the chat of the room client (azl/chat_core.hpp through RoomClient::SetChatSink) against a REAL Azahar room.
//   chat_room_probe <host> [port]        (Tests/integration.sh runs it against its first room)
// Two RoomClients, A and B, each with a ChatSink, and a raw member C that writes the packets a room client of ours never would:
//   1. A sends: B keeps it with A's nick; A keeps its own line (own) and the room sends it nothing back (no echo);
//   2. C sends 20 messages in 0.2 s and A one amid them: B and A decode 2 of C's (one member's share of the 4 a second), the other 18
//      are counted as dropped, and B keeps A's (a flood leaves the others their messages);
//   3. C sends 5000 characters: the room cuts them to 500 bytes, B keeps 128, the last three "...";
//   4. A sends two in a row: the second waits in the box and comes about a second after the first;
//   5. B's chat off (SetChatSink(nullptr)): what C sends is not decoded, and B stays in the room.
//   6. A sends and leaves right after ENet took the message, by Stop() and by the quick stop of EXIT and sleep, 8 times: B gets every one
//      (the goodbye waits for the room's acknowledgement of our last chat message: our line is never one the room did not have).
#include <enet/enet.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "azl/chat_core.hpp"
#include "azl/room_client.hpp"

using namespace azl;

static int g_failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

static void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// The raw member: a real join over its own ENet host, and chat packets written by hand ([0x06][u32 BE length][text]).
struct RawMember {
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    bool joined = false;

    bool Join(const char* address, u16 port, const char* nick) {
        host = enet_host_create(nullptr, 1, room::kNumChannels, 0, 0);
        ENetAddress a{};
        enet_address_set_host(&a, address);
        a.port = port;
        peer = enet_host_connect(host, &a, room::kNumChannels, 0);
        const std::vector<u8> join = room::EncodeJoinRequest(nick, std::string("member-") + nick, kNoPreferredMac, "", "");
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < until && !joined) {
            ENetEvent e;
            if (enet_host_service(host, &e, 10) > 0) {
                if (e.type == ENET_EVENT_TYPE_CONNECT) {
                    enet_peer_send(peer, 0, enet_packet_create(join.data(), join.size(), ENET_PACKET_FLAG_RELIABLE));
                } else if (e.type == ENET_EVENT_TYPE_RECEIVE) {
                    joined = joined || e.packet->data[0] == room::IdJoinSuccess || e.packet->data[0] == room::IdJoinSuccessAsMod;
                    enet_packet_destroy(e.packet);
                }
            }
        }
        return joined;
    }
    void Say(const std::string& text) {
        std::vector<u8> p(5 + text.size());
        p[0] = room::IdChatMessage;
        StoreBE32(p.data() + 1, static_cast<u32>(text.size()));
        std::memcpy(p.data() + 5, text.data(), text.size());
        enet_peer_send(peer, 0, enet_packet_create(p.data(), p.size(), ENET_PACKET_FLAG_RELIABLE));
        enet_host_flush(host);
    }
    void Serve(int ms) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        do {
            ENetEvent e;
            while (enet_host_service(host, &e, 1) > 0) {
                if (e.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(e.packet);
                }
            }
        } while (std::chrono::steady_clock::now() < until);
    }
    void Leave() {
        if (host) {
            enet_peer_disconnect_now(peer, 0);
            enet_host_destroy(host);
            host = nullptr;
        }
    }
};

static bool JoinClient(RoomClient& client, const char* host, u16 port, const char* nick) {
    RoomClient::Config config;
    config.host = host;
    config.port = port;
    config.nickname = nick;
    config.console_id_hash = std::string("chat-probe-") + nick;
    config.game_name = "AzaharLink chat probe";
    client.Start(config);
    for (int i = 0; i < 500 && !client.joined(); ++i) {
        if (client.state() == RoomClient::State::Idle && client.error() != RoomClient::Error::None) {
            break;
        }
        SleepMs(10);
    }
    if (!client.joined()) {
        std::printf("NOT JOINED (%s): %s\n", nick, RoomClient::ErrorName(client.error()));
    }
    return client.joined();
}

/// Waits (serving the raw member) until the sink has kept `taken` messages in all, at most `ms`.
static bool WaitTaken(ChatSink& sink, u32 taken, RawMember& raw, int ms) {
    for (int waited = 0; waited < ms; waited += 10) {
        if (sink.counters.taken.load() >= taken) {
            return true;
        }
        raw.Serve(10);
    }
    return sink.counters.taken.load() >= taken;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <host> [port]\n", argv[0]);
        return 2;
    }
    const char* host = argv[1];
    const u16 port = argc > 2 ? static_cast<u16>(std::atoi(argv[2])) : room::kDefaultPort;
    static ChatSink sink_a, sink_b; // (as on the console: blocks that outlive the network threads)
    RoomClient a, b;
    a.SetChatSink(&sink_a);
    b.SetChatSink(&sink_b);
    RawMember c;
    if (!JoinClient(a, host, port, "ChatProbeA") || !JoinClient(b, host, port, "ChatProbeB") || !c.Join(host, port, "ChatFlood")) {
        std::printf("chat_room_probe: could not join %s:%u\n", host, port);
        return 1;
    }
    c.Serve(300); // (the room's information and the status messages of the joins: the status messages are not the chat's)
    CHECK(sink_a.counters.taken.load() == 0 && sink_b.counters.taken.load() == 0);
    CHECK(sink_a.counters.malformed.load() == 0 && sink_b.counters.malformed.load() == 0);

    std::printf("1. A sends: B keeps it with A's nick, A keeps its own line, the room sends A nothing back\n");
    CHECK(sink_a.Post("Hello from A \xC3\xA8 !", 17));
    CHECK(WaitTaken(sink_b, 1, c, 3000));
    {
        const chat::View& v = sink_b.reader.Refresh(sink_b.ring);
        CHECK(v.count == 1);
        CHECK(std::strcmp(v.lines[0].nick, "ChatProbeA") == 0);
        CHECK(std::strcmp(v.lines[0].text, "Hello from A ? !") == 0);
        CHECK(v.lines[0].own == 0);
        std::printf("   B: %s: %s\n", v.lines[0].nick, v.lines[0].text);
    }
    c.Serve(1200); // (an echo would have come by now; and the second of the rate limit is over)
    {
        const chat::View& v = sink_a.reader.Refresh(sink_a.ring);
        CHECK(v.count == 1 && v.lines[0].own == 1);
        CHECK(std::strcmp(v.lines[0].nick, "ChatProbeA") == 0 && std::strcmp(v.lines[0].text, "Hello from A ? !") == 0);
        CHECK(sink_a.counters.taken.load() == 0); // no echo
        CHECK(sink_a.counters.sent.load() == 1 && sink_a.counters.refused.load() == 0);
    }

    std::printf("2. C sends 20 in 0.2 s, A one amid them: 2 of C's decoded (one member's share), 18 dropped, A's kept\n");
    const u32 b_taken = sink_b.counters.taken.load(), b_drops = sink_b.counters.rate_drops.load();
    const u32 a_taken = sink_a.counters.taken.load(), a_drops = sink_a.counters.rate_drops.load();
    for (int i = 0; i < 20; ++i) {
        char text[32];
        std::snprintf(text, sizeof(text), "flood %d", i);
        c.Say(text);
        c.Serve(10);
        if (i == 9) {
            CHECK(sink_a.Post("amid the flood", 14));
        }
    }
    c.Serve(800);
    CHECK(sink_b.counters.taken.load() - b_taken == 3);
    CHECK(sink_b.counters.rate_drops.load() - b_drops == 18);
    CHECK(sink_a.counters.taken.load() - a_taken == 2);
    CHECK(sink_a.counters.rate_drops.load() - a_drops == 18);
    {
        const chat::View& v = sink_b.reader.Refresh(sink_b.ring);
        CHECK(v.count == 4);
        CHECK(std::strcmp(v.lines[1].text, "flood 0") == 0 && std::strcmp(v.lines[2].text, "flood 1") == 0);
        CHECK(std::strcmp(v.lines[2].nick, "ChatFlood") == 0);
        CHECK(std::strcmp(v.lines[3].text, "amid the flood") == 0 && std::strcmp(v.lines[3].nick, "ChatProbeA") == 0);
        std::printf("   B: taken %u, dropped %u\n", sink_b.counters.taken.load() - b_taken, sink_b.counters.rate_drops.load() - b_drops);
    }

    std::printf("3. C sends 5000 characters: the room cuts them to 500 bytes, B keeps 125 and \"...\"\n");
    c.Serve(1100);
    c.Say(std::string(5000, 'x'));
    CHECK(WaitTaken(sink_b, b_taken + 4, c, 3000));
    {
        const chat::View& v = sink_b.reader.Refresh(sink_b.ring);
        const chat::Line& l = v.lines[v.count - 1];
        CHECK(l.text_len == 128 && std::strlen(l.text) == 128);
        CHECK(std::string(l.text, 125) == std::string(125, 'x') && std::strcmp(l.text + 125, "...") == 0);
        CHECK(std::strcmp(l.nick, "ChatFlood") == 0);
    }

    std::printf("4. A sends two in a row: the second waits in the box, a second after the first\n");
    c.Serve(1100);
    const u32 before = sink_b.counters.taken.load();
    CHECK(sink_a.Post("one", 3));
    for (int i = 0; i < 300 && sink_a.OutboxBusy(); ++i) {
        c.Serve(10);
    }
    CHECK(sink_a.Post("two", 3));
    CHECK(WaitTaken(sink_b, before + 2, c, 4000));
    {
        const chat::View& v = sink_b.reader.Refresh(sink_b.ring);
        const chat::Line& one = v.lines[v.count - 2];
        const chat::Line& two = v.lines[v.count - 1];
        CHECK(std::strcmp(one.text, "one") == 0 && std::strcmp(two.text, "two") == 0);
        CHECK(two.t_ms - one.t_ms >= 900);
        std::printf("   B got them %u ms apart\n", two.t_ms - one.t_ms);
        CHECK(sink_a.counters.sent.load() == 4);
    }

    std::printf("5. B's chat off: nothing decoded, B still in the room\n");
    b.SetChatSink(nullptr);
    c.Serve(1100);
    const u32 off_taken = sink_b.counters.taken.load(), off_drops = sink_b.counters.rate_drops.load();
    const u32 seq = sink_b.ring.seq();
    c.Say("nobody reads this at B");
    c.Serve(1000);
    CHECK(sink_b.counters.taken.load() == off_taken && sink_b.counters.rate_drops.load() == off_drops && sink_b.ring.seq() == seq);
    CHECK(sink_a.counters.taken.load() >= a_taken + 2 + 2); // (A still reads: the 5000 and this one)
    CHECK(b.joined() && a.joined());

    std::printf("6. A sends and leaves at once (Stop, then the quick stop of EXIT and sleep): B gets every one\n");
    b.SetChatSink(&sink_b); // (on again after 5)
    for (int i = 0; i < 8; ++i) {
        const bool quick = (i & 1) != 0;
        if (!a.joined() && !JoinClient(a, host, port, "ChatProbeA")) {
            CHECK(false);
            break;
        }
        c.Serve(1100); // (the second of the rate limit, at A and at B)
        const u32 sent = sink_a.counters.sent.load(), got = sink_b.counters.taken.load();
        char text[32];
        std::snprintf(text, sizeof(text), "%s %d", quick ? "quick bye" : "bye", i);
        CHECK(sink_a.Post(text, static_cast<u32>(std::strlen(text))));
        for (int w = 0; w < 300 && sink_a.counters.sent.load() == sent; ++w) {
            std::this_thread::sleep_for(std::chrono::microseconds(100)); // (no raw member served: A leaves right after ENet took it)
        }
        CHECK(sink_a.counters.sent.load() == sent + 1);
        if (quick) {
            a.RequestStop(true);
        }
        a.Stop();
        CHECK(WaitTaken(sink_b, got + 1, c, 2000));
        const chat::View& v = sink_b.reader.Refresh(sink_b.ring);
        CHECK(v.count > 0 && std::strcmp(v.lines[v.count - 1].text, text) == 0);
    }

    c.Leave();
    a.Stop();
    b.Stop();
    std::printf("chat_room_probe: %s\n", g_failures == 0 ? "OK" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
