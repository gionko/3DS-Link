// Integration test against a REAL azahar-room server (see Tests/integration.sh).
//
//   room_integration <room1-host> <room1-port> <room2-host> <room2-port>
//
// room1: open room. room2: password "secret", max 1 member.
// Every player is a genuine RoomClient (ENet) + UdsEngine; the only thing not real is the game.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include <enet/enet.h>

#include "azl/link_policy.hpp"
#include "azl/log.hpp"
#include "azl/room_client.hpp"
#include "azl/room_members.hpp"
#include "azl/stats.hpp"
#include "azl/uds_engine.hpp"

#include "azahar_peer.hpp"
#include "azahar_rules.hpp"
#include "support.hpp"

using namespace azl;

template <typename Pred>
static bool WaitFor(Pred pred, int timeout_ms = 5000) {
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

struct Player {
    Player(const std::string& host_, u16 port_, const std::string& nick, const std::string& console_id,
           const std::string& password = "")
        : host(host_), port(port_) {
        config.host = host;
        config.port = port;
        config.nickname = nick;
        config.password = password;
        config.console_id_hash = console_id;
        config.game_name = "Test Game";
        config.game_id = 0x0004000000164800ULL;

        UdsEngine::Transport transport;
        transport.send = [this](WifiPacket& p) {
            if (!room.joined()) {
                return;
            }
            p.transmitter_address = room.mac();
            g_rules.Inspect(p);
            room.SendWifiPacket(p);
        };
        transport.get_mac = [this] { return room.mac(); };
        engine.reset(new UdsEngine(&kernel, transport));
        room.SetWifiCallback([this](const WifiPacket& p) { engine->OnWifiPacket(p); });
        room.SetTickCallback([this](u64 now_ms) { engine->Tick(now_ms); });
    }
    ~Player() {
        room.Stop();
    }

    bool Join() {
        room.Start(config);
        const bool joined = WaitFor([this] {
            return room.state() == RoomClient::State::Joined ||
                   (room.state() == RoomClient::State::Idle && room.error() != RoomClient::Error::None);
        }, 8000) && room.joined();
        if (joined) {
            g_rules.AddMember(room.mac());
        }
        return joined;
    }
    RoomClient::Error JoinError() {
        room.Start(config);
        WaitFor([this] { return room.state() == RoomClient::State::Idle && room.error() != RoomClient::Error::None; }, 8000);
        return room.error();
    }

    std::string host;
    u16 port;
    RoomClient::Config config;
    RoomClient room;
    FakeKernel kernel;
    std::unique_ptr<UdsEngine> engine;
};

static void TestRoomBasics(const std::string& host, u16 port, const std::string& host2, u16 port2) {
    std::printf("room server: join / errors\n");

    Player alice(host, port, "AliceTest", "3ds-alice");
    CHECK(alice.Join());
    CHECK(alice.room.joined());
    const Mac amac = alice.room.mac();
    CHECK_EQ(amac[0], 0x00);
    CHECK_EQ(amac[1], 0x1F);
    CHECK_EQ(amac[2], 0x32);
    const auto info = alice.room.room_info();
    CHECK_EQ(info.members.size(), std::size_t(1));
    CHECK(info.members.size() == 1 && info.members[0].nickname == "AliceTest");
    CHECK(info.members.size() == 1 && info.members[0].game_name == "Test Game");
    CHECK(info.members.size() == 1 && info.members[0].game_id == 0x0004000000164800ULL);
    CHECK(info.members.size() == 1 && info.members[0].mac == amac);

    Player bob(host, port, "BobTester", "3ds-bob");
    CHECK(bob.Join());
    CHECK(bob.room.mac() != amac);
    CHECK(WaitFor([&] { return alice.room.room_info().members.size() == 2; }));
    CHECK_EQ(bob.room.room_info().members.size(), std::size_t(2));

    // C7: the lines the plugin's log has for the player that came in and the one that left, made from the lists this real room
    // sent (the nickname, the title the room has for the player, the MAC the room gave it, how many players of how many)
    const auto member_rows = [](const room::RoomInfo& i) {
        std::vector<RoomMemberRow> rows;
        for (const room::MemberInfo& m : i.members) {
            rows.push_back({m.mac, m.nickname, m.game_name});
        }
        return rows;
    };
    const auto mac_text = [](const Mac& m) {
        char text[24];
        std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
        return std::string(text);
    };
    const room::RoomInfo with_bob = alice.room.room_info();
    CHECK_EQ(with_bob.member_slots, 8u); // (the room is started with --max_members 8)
    {
        const std::vector<std::string> lines = RoomMemberLines(member_rows(info), member_rows(with_bob), with_bob.member_slots);
        CHECK_EQ(lines.size(), std::size_t(1));
        CHECK(lines.size() == 1 &&
              lines[0] == "room: 'BobTester' [Test Game] joined the room (MAC " + mac_text(bob.room.mac()) + "): 2/8 players");
        CHECK(RoomMemberLines(member_rows(with_bob), member_rows(alice.room.room_info()), with_bob.member_slots).empty());
    }

    // Same nickname -> collision; too short nickname -> rejected the same way
    {
        Player dup(host, port, "AliceTest", "3ds-dup");
        CHECK(dup.JoinError() == RoomClient::Error::NameCollision);
        Player shortnick(host, port, "abc", "3ds-short");
        CHECK(shortnick.JoinError() == RoomClient::Error::NameCollision);
    }
    // Same console id -> collision
    {
        Player dup(host, port, "OtherName", "3ds-alice");
        CHECK(dup.JoinError() == RoomClient::Error::ConsoleIdCollision);
    }
    // Nobody listening -> could not connect
    {
        Player nobody(host, 1, "NoRoomHere", "3ds-nobody");
        CHECK(nobody.JoinError() == RoomClient::Error::CouldNotConnect);
    }
    // Password room with two slots
    {
        Player wrong(host2, port2, "WrongPass", "3ds-wp", "nope");
        CHECK_EQ(std::string(RoomClient::ErrorName(wrong.JoinError())), std::string(RoomClient::ErrorName(RoomClient::Error::WrongPassword)));
        Player right(host2, port2, "RightPass", "3ds-rp", "secret");
        CHECK(right.Join());
        if (!right.room.joined()) {
            std::fprintf(stderr, "  right.Join: state=%s error=%s\n",
                         RoomClient::StateName(right.room.state()),
                         RoomClient::ErrorName(right.room.error()));
        }
        Player second(host2, port2, "SecondSeat", "3ds-second", "secret");
        CHECK(second.Join());
        Player full(host2, port2, "NoSlotLeft", "3ds-full", "secret");
        CHECK_EQ(std::string(RoomClient::ErrorName(full.JoinError())), std::string(RoomClient::ErrorName(RoomClient::Error::RoomIsFull)));
    }

    // Leaving updates the member list seen by the others
    const Mac bob_mac = bob.room.mac();
    bob.room.Stop();
    CHECK(WaitFor([&] { return alice.room.room_info().members.size() == 1; }));
    {
        const std::vector<std::string> lines = RoomMemberLines(member_rows(with_bob), member_rows(alice.room.room_info()), with_bob.member_slots);
        CHECK_EQ(lines.size(), std::size_t(1));
        CHECK(lines.size() == 1 &&
              lines[0] == "room: 'BobTester' [Test Game] left the room (MAC " + mac_text(bob_mac) + "): 1/8 players");
    }

    // The room keeps the frame untouched: broadcast reaches everyone but the sender
    Player carol(host, port, "CarolTest", "3ds-carol");
    CHECK(carol.Join());
    std::atomic<int> got_by_alice{0}, got_by_carol{0};
    alice.room.SetWifiCallback([&](const WifiPacket& p) {
        if (p.type == WifiPacket::Type::NodeMap && p.data.size() == 3) {
            ++got_by_alice;
        }
    });
    carol.room.SetWifiCallback([&](const WifiPacket& p) {
        if (p.type == WifiPacket::Type::NodeMap && p.data.size() == 3) {
            ++got_by_carol;
        }
    });
    WifiPacket p;
    p.type = WifiPacket::Type::NodeMap;
    p.channel = 11;
    p.transmitter_address = alice.room.mac();
    p.destination_address = kBroadcastMac;
    p.data = {1, 2, 3};
    alice.room.SendWifiPacket(p);
    CHECK(WaitFor([&] { return got_by_carol == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(got_by_alice.load(), 0); // not echoed to the sender
    // Unicast reaches only the addressee
    p.destination_address = carol.room.mac();
    alice.room.SendWifiPacket(p);
    CHECK(WaitFor([&] { return got_by_carol == 2; }));
    CHECK_EQ(got_by_alice.load(), 0);
}

static void TestUdsOverRealRoom(const std::string& host, u16 port) {
    std::printf("uds engines over the real room server\n");
    g_rules.Reset();
    Player hostp(host, port, "UdsHost01", "3ds-uds-host");
    Player client(host, port, "UdsClient1", "3ds-uds-client");
    CHECK(hostp.Join());
    CHECK(client.Join());
    if (!hostp.room.joined() || !client.room.joined()) {
        std::fprintf(stderr, "  (cannot continue without both players in the room)\n");
        return;
    }

    NodeInfo n = MakeNode(0xAAAA, "Host", 0);
    u32 ev;
    CHECK_EQ(hostp.engine->Initialize(n, kTestSharedMemory, kTestVersion, &ev), 0u);
    n = MakeNode(0xBBBB, "Client", 0);
    CHECK_EQ(client.engine->Initialize(n, kTestSharedMemory, kTestVersion, &ev), 0u);

    NetworkInfo ni = MakeNetworkInfo(4, 4);
    CHECK_EQ(hostp.engine->BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni)), 0u);

    // The client scans until the host's beacon arrives (they are sent every ~102 ms).
    std::vector<u8> scan(0x4000);
    u32 entries = 0;
    CHECK(WaitFor([&] {
        client.engine->RecvBeaconBroadcastData(scan.data(), static_cast<u32>(scan.size()), kBroadcastMac);
        entries = LoadLE32(scan.data() + 8);
        return entries > 0;
    }, 5000));
    CHECK_EQ(entries, 1u);
    const u8* entry = scan.data() + kBeaconDataReplyHeaderSize;
    const u32 entry_size = LoadLE32(entry);
    CHECK_EQ(Hex(entry + 8, 6), Hex(hostp.room.mac().data(), 6)); // beacon sent from the host's room MAC

    // Rebuild the network info like libctru does and connect
    std::vector<u8> beacon(entry + kBeaconEntryHeaderSize, entry + entry_size);
    NetworkInfo scanned;
    std::memset(&scanned, 0, sizeof(scanned));
    const u8* net_tag = nullptr;
    for (std::size_t pos = 12; pos + 2 <= beacon.size();) {
        const u8 id = beacon[pos], len = beacon[pos + 1];
        pos += 2;
        if (id == 0xdd && std::memcmp(&beacon[pos], kNintendoOui, 3) == 0 && beacon[pos + 3] == 21) {
            net_tag = &beacon[pos];
        }
        pos += len;
    }
    CHECK(net_tag != nullptr);
    std::memcpy(scanned.oui_value, net_tag, 0x1F);
    scanned.channel = entry[5];
    std::memcpy(scanned.host_mac_address.data(), entry + 8, 6);

    // Bind before connecting so that no data is missed
    u32 host_bind = 0, client_bind = 0;
    CHECK_EQ(hostp.engine->Bind(2, 0x5F4, 1, kBroadcastNetworkNodeId, &host_bind), 0u);
    CHECK_EQ(client.engine->Bind(2, 0x5F4, 1, kBroadcastNetworkNodeId, &client_bind), 0u);

    CHECK_EQ(client.engine->ConnectToNetwork(reinterpret_cast<u8*>(&scanned), sizeof(scanned), 1), 0u);
    CHECK(client.engine->status() == NetworkStatus::ConnectedAsClient);
    CHECK(WaitFor([&] { return hostp.engine->GetConnectionStatus().total_nodes == 2; }));

    // Data both ways through the real server
    const u8 ping[] = "ping from the client";
    CHECK_EQ(client.engine->SendTo(1, 1, sizeof(ping), 0, ping, sizeof(ping)), 0u);
    std::vector<u8> out;
    u32 size = 0;
    u16 src = 0;
    CHECK(WaitFor([&] {
        CHECK_EQ(PullInto(*hostp.engine, 2, &out, &size, &src), 0u);
        return size != 0;
    }));
    CHECK_EQ(size, u32(sizeof(ping)));
    CHECK_EQ(src, 2);
    CHECK(std::memcmp(out.data(), ping, sizeof(ping)) == 0);

    const u8 pong[] = "pong from the host";
    CHECK_EQ(hostp.engine->SendTo(2, 1, sizeof(pong), 0, pong, sizeof(pong)), 0u);
    CHECK(WaitFor([&] {
        CHECK_EQ(PullInto(*client.engine, 2, &out, &size, &src), 0u);
        return size != 0;
    }));
    CHECK_EQ(size, u32(sizeof(pong)));
    CHECK_EQ(src, 1);
    CHECK(std::memcmp(out.data(), pong, sizeof(pong)) == 0);

    // Largest legal payload
    std::vector<u8> big(0x5C6);
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<u8>(i * 7);
    }
    CHECK_EQ(client.engine->SendTo(1, 1, static_cast<u32>(big.size()), 0, big.data(), big.size()), 0u);
    CHECK(WaitFor([&] {
        CHECK_EQ(PullInto(*hostp.engine, 2, &out, &size, &src), 0u);
        return size != 0;
    }));
    CHECK_EQ(size, u32(0x5C6));
    CHECK(std::memcmp(out.data(), big.data(), big.size()) == 0);

    // A burst keeps its order
    for (u8 i = 0; i < 50; ++i) {
        CHECK_EQ(client.engine->SendTo(1, 1, 1, 0, &i, 1), 0u);
    }
    int received = 0;
    CHECK(WaitFor([&] {
        for (;;) {
            CHECK_EQ(PullInto(*hostp.engine, 2, &out, &size, &src), 0u);
            if (size == 0) {
                break;
            }
            CHECK_EQ(out[0], received);
            ++received;
        }
        return received == 50;
    }));

    // Client leaves, host notices
    CHECK_EQ(client.engine->DisconnectNetwork(), 0u);
    CHECK(WaitFor([&] { return hostp.engine->GetConnectionStatus().total_nodes == 1; }));
    CHECK_EQ(hostp.engine->DestroyNetwork(), 0u);

    // Everything both engines sent through the real server obeyed the Azahar-peer rules
    CHECK(g_rules.inspected() > 50);
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------
// The bounded TX queue over real ENet (spec F20): a room server of our own, in process, that can stall
// ---------------------------------------------------------------------------------------------
namespace {

// Accepts the ENet connection, answers the join request and records every 802.11 frame message it gets
// (type and the sequence number the test put in the first two bytes of the data). Stall(true) makes it
// stop servicing its socket altogether: no acknowledgements, nothing read, like a server whose link to
// us died.
class StubRoom {
public:
    struct Got {
        u8 type;
        u16 seq;
        const void* peer = nullptr; // the connection it came through (the server's end: one per client session)
    };
    StubRoom() {
        ENetAddress address{};
        address.host = ENET_HOST_ANY;
        address.port = 0;
        host_ = enet_host_create(&address, 4, room::kNumChannels, 0, 0);
        if (host_) {
            ENetAddress bound{};
            enet_socket_get_address(host_->socket, &bound);
            port_ = bound.port;
            thread_ = std::thread([this] { Loop(); });
        }
    }
    ~StubRoom() {
        stop_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
        if (host_) {
            enet_host_destroy(host_);
        }
    }
    u16 port() const {
        return port_;
    }
    /// The server's own datagram socket (a test that looks for the client's among the process' sockets skips it).
    int socket() const {
        return host_ ? static_cast<int>(host_->socket) : -1;
    }
    /// ENet connections ever accepted.
    u32 Connects() const {
        return connects_.load();
    }
    void Stall(bool on) {
        parked_ = false;
        stalled_ = on;
    }
    /// True once the server thread has noticed the stall: from here on nothing is read or acknowledged.
    bool Parked() const {
        return parked_.load();
    }
    /// Sends the client an unreliable datagram every 200 us and reads nothing, so that none of what the client
    /// sends is ever acknowledged while the client has something to read all the time.
    void Flood(bool on) {
        flood_started_ = false;
        flooding_ = on;
    }
    /// True once the server thread is in the flood: it reads nothing from here on.
    bool FloodStarted() const {
        return flood_started_.load();
    }
    std::vector<Got> Frames() {
        std::lock_guard<std::mutex> l(m_);
        return got_;
    }
    std::size_t Count() {
        std::lock_guard<std::mutex> l(m_);
        return got_.size();
    }
    void Forget() {
        std::lock_guard<std::mutex> l(m_);
        got_.clear();
    }
    /// Sends a message to the client (reliable), from the server's own thread.
    void SendToClient(std::vector<u8> message) {
        std::lock_guard<std::mutex> l(m_);
        outbox_.push_back(std::move(message));
    }

private:
    void Loop() {
        while (!stop_.load()) {
            if (stalled_.load()) {
                parked_ = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (flooding_.load()) {
                flood_started_ = true;
                if (peer_) {
                    static const u8 junk[40] = {};
                    enet_peer_send(peer_, 0, enet_packet_create(junk, sizeof(junk), 0));
                    enet_host_flush(host_);
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }
            std::vector<std::vector<u8>> outgoing;
            {
                std::lock_guard<std::mutex> l(m_);
                outgoing.swap(outbox_);
            }
            for (const auto& message : outgoing) {
                if (peer_) {
                    enet_peer_send(peer_, 0, enet_packet_create(message.data(), message.size(), ENET_PACKET_FLAG_RELIABLE));
                }
            }
            ENetEvent event;
            while (!stalled_.load() && enet_host_service(host_, &event, 2) > 0) {
                if (event.type == ENET_EVENT_TYPE_CONNECT) {
                    peer_ = event.peer;
                    ++connects_;
                } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    OnMessage(event.peer, event.packet->data, event.packet->dataLength);
                    enet_packet_destroy(event.packet);
                }
            }
        }
    }
    void OnMessage(ENetPeer* peer, const u8* data, std::size_t size) {
        if (size == 0) {
            return;
        }
        if (data[0] == room::IdJoinRequest) {
            // JoinSuccess: the id and the MAC the room gives the member
            const u8 reply[] = {room::IdJoinSuccess, 0x00, 0x1F, 0x32, 0x00, 0x00, 0x99};
            enet_peer_send(peer, 0, enet_packet_create(reply, sizeof(reply), ENET_PACKET_FLAG_RELIABLE));
        } else if (data[0] == room::IdWifiPacket) {
            constexpr std::size_t kSeqAt = 19 + kLlcHeaderSize + kSecureDataHeaderSize; // first data byte
            Got g{size > 1 ? data[1] : u8(0xFF), 0, peer};
            if (g.type == static_cast<u8>(WifiPacket::Type::Data) && size >= kSeqAt + 2) {
                g.seq = LoadBE16(data + kSeqAt);
            }
            std::lock_guard<std::mutex> l(m_);
            got_.push_back(g);
        }
    }

    ENetHost* host_ = nullptr;
    ENetPeer* peer_ = nullptr;
    u16 port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<bool> stalled_{false};
    std::atomic<bool> parked_{false};
    std::atomic<bool> flooding_{false};
    std::atomic<bool> flood_started_{false};
    std::atomic<u32> connects_{0};
    std::mutex m_;
    std::vector<Got> got_;
    std::vector<std::vector<u8>> outbox_;
    std::thread thread_;
};

WifiPacket TxFrame(u16 seq, u32 data_size) {
    std::vector<u8> data(data_size < 2 ? 2 : data_size);
    StoreBE16(data.data(), seq);
    WifiPacket p;
    p.type = WifiPacket::Type::Data;
    p.channel = 11;
    p.destination_address = kBroadcastMac;
    p.data = GenerateDataPayload(data.data(), data.size(), 1, kBroadcastNetworkNodeId, 2, seq);
    return p;
}

WifiPacket TxDeauth() {
    WifiPacket p;
    p.type = WifiPacket::Type::Deauthentication;
    p.channel = 11;
    p.destination_address = kBroadcastMac;
    return p;
}

// A client of the stub whose network thread can be held inside its tick callback.
struct TxClient {
    explicit TxClient(StubRoom& server, bool fast_data) {
        config.host = "127.0.0.1";
        config.port = server.port();
        config.nickname = "TxQueueTest";
        config.console_id_hash = "3ds-txqueue";
        config.game_name = "Test Game";
        config.game_id = 0x0004000000164800ULL;
        room.SetFastData(fast_data);
        room.SetTickCallback([this](u64) {
            if (held.load()) {
                in_hold = true;
                while (held.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                in_hold = false;
            }
        });
    }
    ~TxClient() {
        held = false;
        room.Stop();
    }
    bool Join() {
        room.Start(config);
        return WaitFor([this] { return room.joined(); });
    }
    /// Holds the network thread (it takes nothing from the queue while held).
    bool Hold() {
        held = true;
        return WaitFor([this] { return in_hold.load(); });
    }
    void Release() {
        held = false;
    }
    RoomClient::Config config;
    RoomClient room;
    std::atomic<bool> held{false};
    std::atomic<bool> in_hold{false};
};

} // namespace

// The ENet host of the client takes packets of 64 KB at the most (its default is 32 MB, and a failed
// allocation aborts the plugin): a bigger message from the room is discarded, ENet's own checks drop it,
// and everything below the limit, fragmented or not, is delivered as before. (The reliable channel waits
// for the message that never gets acknowledged, so a room that sends such a thing loses its link at ENet's
// timeout: a room never does, and an error is better than an abort.)
static void TestEnetPacketLimit() {
    std::printf("enet host limits: a message over 64 KB is discarded, one under it arrives\n");
    StubRoom server;
    TxClient client(server, false);
    std::mutex m;
    std::vector<std::size_t> sizes;
    client.room.SetWifiCallback([&](const WifiPacket& p) {
        std::lock_guard<std::mutex> l(m);
        sizes.push_back(p.data.size());
    });
    CHECK(client.Join());
    auto message = [](std::size_t payload) {
        WifiPacket p;
        p.type = WifiPacket::Type::Beacon; // (any type reaches the callback)
        p.channel = 11;
        p.transmitter_address = Mac{0, 0x1F, 0x32, 0, 0, 0x42};
        p.destination_address = kBroadcastMac;
        p.data.assign(payload, 0x5A);
        return room::EncodeWifiPacket(p);
    };
    server.SendToClient(message(1000));
    server.SendToClient(message(50000));  // 36 fragments, but under the limit: delivered
    CHECK(WaitFor([&] {
        std::lock_guard<std::mutex> l(m);
        return sizes.size() >= 2;
    }, 8000));
    server.SendToClient(message(100000)); // over the limit: never delivered
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    std::lock_guard<std::mutex> l(m);
    CHECK_EQ(sizes.size(), std::size_t(2));
    if (sizes.size() == 2) {
        CHECK_EQ(sizes[0], std::size_t(1000));
        CHECK_EQ(sizes[1], std::size_t(50000));
    }
    CHECK(client.room.joined()); // (no abort, no crash; the sanitizers watch the rest)
}

static void TestTxQueueStalledServer() {
    std::printf("tx queue over ENet: bounded, oldest game frame first, stale frames dropped, a stalled server\n");
    // 1. The network thread is held: the game keeps sending. 128 game frames and 32 handshake frames are
    //    all that is kept, the NEWEST game frames, and they go out in order once the thread runs again.
    {
        StubRoom server;
        CHECK(server.port() != 0);
        TxClient client(server, false);
        CHECK(client.Join());
        CHECK(WaitFor([&] { return client.room.queued_frames() == 0; }));
        server.Forget();
        CHECK(client.Hold());
        std::memset(&g_stats, 0, sizeof(g_stats));
        for (u16 seq = 0; seq < 400; ++seq) {
            client.room.SendWifiPacket(TxFrame(seq, 100));
        }
        for (int i = 0; i < 40; ++i) {
            client.room.SendWifiPacket(TxDeauth());
        }
        CHECK_EQ(client.room.queued_frames(), std::size_t(128 + 32));
        CHECK(client.room.queued_bytes() <= 128 * 141 + 32 * 19);
        CHECK_EQ(g_stats.tx_cap_drops, 272u + 8u);
        CHECK_EQ(g_stats.tx_stale_drops, 0u);
        CHECK_EQ(g_stats.queue_max, 160u);
        client.Release();
        CHECK(WaitFor([&] { return server.Count() >= 160; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto got = server.Frames();
        CHECK_EQ(got.size(), std::size_t(160));
        for (std::size_t i = 0; i < got.size() && i < 160; ++i) {
            if (i < 128) {
                CHECK(got[i].type == 1 && got[i].seq == 272 + i); // the newest 128, in order
            } else {
                CHECK_EQ(got[i].type, 4); // then the handshake frames that were kept
            }
        }
    }
    // 2. The byte cap: frames of 600 bytes (641 on the wire) fill 32 KB after 51
    {
        StubRoom server;
        TxClient client(server, true); // (unreliable data)
        CHECK(client.Join());
        CHECK(WaitFor([&] { return client.room.queued_frames() == 0; }));
        server.Forget();
        CHECK(client.Hold());
        std::memset(&g_stats, 0, sizeof(g_stats));
        for (u16 seq = 0; seq < 120; ++seq) {
            client.room.SendWifiPacket(TxFrame(seq, 600));
        }
        CHECK_EQ(client.room.queued_frames(), std::size_t(51));
        CHECK(client.room.queued_bytes() <= 32768);
        CHECK_EQ(g_stats.tx_cap_drops, 69u);
        CHECK(g_stats.tx_queue_bytes_max <= 32768);
        client.Release();
        CHECK(WaitFor([&] { return client.room.queued_frames() == 0; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        // unreliable frames may be lost on the loopback in theory; in practice every one arrives, in order
        const auto got = server.Frames();
        CHECK(got.size() >= 40 && got.size() <= 51);
        for (std::size_t i = 1; i < got.size(); ++i) {
            CHECK(got[i].seq > got[i - 1].seq);
        }
        CHECK(!got.empty() && got.back().seq == 119); // the newest got through
    }
    // 3. A burst that waited more than a second is stale: only the fresh frames go
    {
        StubRoom server;
        TxClient client(server, false);
        CHECK(client.Join());
        CHECK(WaitFor([&] { return client.room.queued_frames() == 0; }));
        server.Forget();
        CHECK(client.Hold());
        std::memset(&g_stats, 0, sizeof(g_stats));
        for (u16 seq = 0; seq < 10; ++seq) {
            client.room.SendWifiPacket(TxFrame(seq, 100));
        }
        client.room.SendWifiPacket(TxDeauth()); // the handshake never goes stale
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        for (u16 seq = 100; seq < 105; ++seq) {
            client.room.SendWifiPacket(TxFrame(seq, 100));
        }
        client.Release();
        CHECK(WaitFor([&] { return server.Count() >= 6; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto got = server.Frames();
        CHECK_EQ(got.size(), std::size_t(6));
        int data = 0, deauth = 0;
        for (const auto& g : got) {
            data += g.type == 1 && g.seq >= 100;
            deauth += g.type == 4;
        }
        CHECK_EQ(data, 5);
        CHECK_EQ(deauth, 1);
        CHECK_EQ(g_stats.tx_stale_drops, 10u);
        CHECK_EQ(g_stats.tx_cap_drops, 0u);
    }
    // 4. A stalled server: nothing is acknowledged, so reliable data piles up in ENet. Past 64 KB of it,
    //    game data stops going in (and is counted), the handshake still goes, memory stays bounded.
    {
        StubRoom server;
        TxClient client(server, false);
        CHECK(client.Join());
        CHECK(WaitFor([&] { return client.room.queued_frames() == 0; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        server.Forget();
        server.Stall(true);
        CHECK(WaitFor([&] { return server.Parked(); })); // (not a frame more is acknowledged from here on)
        std::memset(&g_stats, 0, sizeof(g_stats));
        for (u16 seq = 0; seq < 600; ++seq) {
            client.room.SendWifiPacket(TxFrame(seq, 300));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(WaitFor([&] { return client.room.queued_frames() == 0; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // 341 bytes each: 193 are handed to ENet before its backlog reaches 64 KB, the rest is not (a margin for
        // the bytes ENet counts a little differently)
        CHECK(g_stats.tx_reliable >= 150 && g_stats.tx_reliable <= 200);
        CHECK_EQ(g_stats.tx_reliable + g_stats.tx_cap_drops + g_stats.tx_stale_drops, 600u);
        CHECK(g_stats.tx_cap_drops >= 400);
        CHECK(g_stats.tx_queue_bytes_max <= 32768);
        // a handshake frame is not game data: it goes though the backlog is full
        const u32 before = g_stats.tx_reliable;
        client.room.SendWifiPacket(TxDeauth());
        CHECK(WaitFor([&] { return g_stats.tx_reliable == before + 1; }));
        // and the server coming back gets what is in ENet's hands, in order, nothing newer than the cap
        server.Stall(false);
        CHECK(WaitFor([&] { return server.Count() >= before + 1; }, 10000));
        const auto got = server.Frames();
        std::size_t data_frames = 0;
        for (std::size_t i = 0; i < got.size(); ++i) {
            if (got[i].type == 1) {
                CHECK_EQ(got[i].seq, data_frames); // the first ones, none missing in between
                ++data_frames;
            }
        }
        CHECK_EQ(data_frames, std::size_t(before));
    }
}

// ---------------------------------------------------------------------------------------------
// The room link and the game's session over the real room (spec F07, F15, R3, R7)
// ---------------------------------------------------------------------------------------------
namespace {

// A UDP forwarder between a client and the room server that can be cut and opened: what an access point that is
// switched off and on again looks like to the client, and to the server (which gets nothing from it any more).
class UdpGate {
public:
    UdpGate(const std::string& server_host, u16 server_port) {
        addrinfo hints{};
        addrinfo* found = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(server_host.c_str(), std::to_string(server_port).c_str(), &hints, &found) != 0 || !found) {
            return;
        }
        std::memcpy(&server_, found->ai_addr, sizeof(server_));
        freeaddrinfo(found);
        in_ = socket(AF_INET, SOCK_DGRAM, 0);
        out_ = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(in_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            return;
        }
        socklen_t length = sizeof(local);
        getsockname(in_, reinterpret_cast<sockaddr*>(&local), &length);
        port_ = ntohs(local.sin_port);
        thread_ = std::thread([this] { Loop(); });
    }
    ~UdpGate() {
        stop_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
        close(in_);
        close(out_);
    }
    u16 port() const {
        return port_;
    }
    /// While cut, nothing goes through in either direction.
    void Cut(bool cut) {
        cut_ = cut;
    }

private:
    void Loop() {
        std::vector<u8> buffer(4096);
        while (!stop_.load()) {
            pollfd fds[2] = {{in_, POLLIN, 0}, {out_, POLLIN, 0}};
            if (poll(fds, 2, 5) <= 0) {
                continue;
            }
            if (fds[0].revents & POLLIN) {
                sockaddr_in from{};
                socklen_t length = sizeof(from);
                const ssize_t n = recvfrom(in_, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&from), &length);
                if (n > 0) {
                    client_ = from;
                    have_client_ = true;
                    if (!cut_.load()) {
                        sendto(out_, buffer.data(), static_cast<std::size_t>(n), 0, reinterpret_cast<sockaddr*>(&server_), sizeof(server_));
                    }
                }
            }
            if (fds[1].revents & POLLIN) {
                const ssize_t n = recvfrom(out_, buffer.data(), buffer.size(), 0, nullptr, nullptr);
                if (n > 0 && have_client_ && !cut_.load()) {
                    sendto(in_, buffer.data(), static_cast<std::size_t>(n), 0, reinterpret_cast<sockaddr*>(&client_), sizeof(client_));
                }
            }
        }
    }

    int in_ = -1, out_ = -1;
    u16 port_ = 0;
    sockaddr_in server_{};
    sockaddr_in client_{};
    bool have_client_ = false;
    std::atomic<bool> stop_{false};
    std::atomic<bool> cut_{false};
    std::thread thread_;
};

// A console as the plugin runs it: the room client and the engine, wired the way Plugin does it (the state callback
// asks the LossGuard, the room's member lists go to the engine, a "frame" thread runs the RejoinPolicy), with the
// counters a test needs. Its network thread can be held in its tick callback.
class Supervised {
public:
    Supervised(const std::string& host, u16 port, const std::string& nick, const std::string& console_id, const Mac& mac)
        : nickname_(nick) {
        config_.host = host;
        config_.port = port;
        config_.nickname = nick;
        config_.console_id_hash = console_id;
        config_.game_name = "Test Game";
        config_.game_id = 0x0004000000164800ULL;
        config_.preferred_mac = mac;
        launch_ms_ = platform::NowMs();

        UdsEngine::Transport transport;
        transport.send = [this](WifiPacket& p) {
            if (!room.joined()) {
                return;
            }
            p.transmitter_address = room.mac();
            g_rules.Inspect(p);
            room.SendWifiPacket(p);
        };
        transport.get_mac = [this] { return room.joined() ? room.mac() : config_.preferred_mac; };
        engine.reset(new UdsEngine(&kernel, transport));
        room.SetWifiCallback([this](const WifiPacket& p) { engine->OnWifiPacket(p); });
        room.SetTickCallback([this](u64 now_ms) {
            engine->Tick(now_ms);
            if (const u32 ms = hold_ms_.exchange(0)) {
                in_hold = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                in_hold = false;
            }
        });
        room.SetRoomInfoCallback([this](const room::RoomInfo& info) {
            std::vector<Mac> members;
            for (const room::MemberInfo& m : info.members) {
                members.push_back(m.mac);
            }
            engine->OnRoomMembers(members);
        });
        room.SetStateCallback([this](RoomClient::State state, RoomClient::Error error) { OnState(state, error); });
    }
    ~Supervised() {
        Quit();
        room.Stop();
    }

    /// Begins supervising: "Connect to room" is on.
    void Start() {
        want_ = true;
        if (!frame_.joinable()) {
            frame_ = std::thread([this] {
                while (!stop_.load()) {
                    Frame();
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            });
        }
    }
    /// Stops supervising (the thread of the frames); the room client is left as it is.
    void Quit() {
        stop_ = true;
        if (frame_.joinable()) {
            frame_.join();
        }
    }
    /// The next round of the network thread lasts `ms` longer.
    void HoldNetworkThread(u32 ms) {
        hold_ms_ = ms;
    }
    void SetLaunchLongAgo() {
        launch_ms_ = platform::NowMs() - 100000;
    }
    void SetPassword(const std::string& password) {
        config_.password = password;
    }
    /// The times (platform::NowMs) the policy asked for an attempt to join.
    std::vector<u64> AttemptTimes() {
        std::lock_guard<std::mutex> l(attempts_m_);
        return attempt_ms_;
    }

    RoomClient room;
    FakeKernel kernel;
    std::unique_ptr<UdsEngine> engine;
    LossGuard guard;
    RejoinPolicy rejoin;
    u32 status_event = 0;
    std::atomic<u32> joins{0}, lefts{0}, deferred{0}, resumed{0}, drop_calls{0}, drops{0}, collisions{0};
    std::atomic<u32> last_stall_ms{0};
    std::atomic<int> last_error{0};
    std::atomic<bool> in_hold{false};
    std::atomic<u8> wifi_state{RejoinPolicy::kWifiConnected}; ///< the network state byte of the shared page, as the test sets it
    std::atomic<u32> attempts{0};                              ///< attempts to join the policy asked for

private:
    void OnState(RoomClient::State state, RoomClient::Error error) {
        Log("room (%s): %s%s%s", config_.nickname.c_str(), RoomClient::StateName(state), error != RoomClient::Error::None ? " - " : "",
            error != RoomClient::Error::None ? RoomClient::ErrorName(error) : "");
        if (state == RoomClient::State::Connecting) {
            engine->ResetRoomPresence();
        }
        const u64 now = platform::NowMs();
        const bool left = state != RoomClient::State::Joined && joined_.exchange(false);
        if (state == RoomClient::State::Idle && error != RoomClient::Error::None) {
            last_error = static_cast<int>(error);
            collisions += error == RoomClient::Error::NameCollision || error == RoomClient::Error::MacCollision ||
                          error == RoomClient::Error::ConsoleIdCollision;
        }
        if (left) {
            ++lefts;
            engine->ResetRoomPresence();
            if (error != RoomClient::Error::None) {
                rejoin.NoteLoss(now);
            }
            const u32 stall = room.RecentStallMs(now, LossGuard::kLookBackMs);
            last_stall_ms = stall;
            if (guard.OnLeft(error, stall, room.mac(), nickname_, now) == LossGuard::Verdict::Drop) {
                DropSession();
            } else {
                ++deferred;
            }
        }
        if (state == RoomClient::State::Joined) {
            joined_ = true;
            ++joins;
            g_rules.AddMember(room.mac());
            rejoin.NoteJoined();
            switch (guard.OnJoined(room.mac(), nickname_, now)) {
            case LossGuard::Outcome::Kept:
                ++resumed;
                break;
            case LossGuard::Outcome::Drop:
                DropSession();
                break;
            case LossGuard::Outcome::Nothing:
                break;
            }
        }
    }
    void DropSession() {
        ++drop_calls;
        if (engine->OnTransportDown(NetworkStatusChangeReason::ConnectionLost)) {
            ++drops;
        }
    }
    void Frame() {
        const u64 now = platform::NowMs();
        if (guard.Due(now)) {
            DropSession();
        }
        RejoinPolicy::Input in;
        in.want_connected = want_;
        in.state = room.state();
        in.error = room.error();
        in.now_ms = now;
        in.launch_ms = launch_ms_;
        in.nickname_automatic = false;
        in.wifi_state = wifi_state.load();
        const RejoinPolicy::Step step = rejoin.Update(in);
        if (step.disconnect) {
            room.Stop();
        }
        if (step.clear_error) {
            room.ClearError();
        }
        if (!want_ || !step.alive) {
            if (guard.Settle()) {
                DropSession();
            }
            return;
        }
        if (step.connect) {
            {
                std::lock_guard<std::mutex> l(attempts_m_);
                attempt_ms_.push_back(now);
            }
            ++attempts;
            RoomClient::Config config = config_;
            nickname_ = config.nickname = config_.nickname + (rejoin.nickname_suffixed() ? "-sx" : "");
            if (!rejoin.prefer_real_mac()) {
                config.preferred_mac = kNoPreferredMac;
            }
            room.Start(config);
        }
    }

    RoomClient::Config config_;
    std::string nickname_;
    u64 launch_ms_ = 0;
    bool want_ = false;
    std::atomic<bool> joined_{false};
    std::atomic<bool> stop_{false};
    std::atomic<u32> hold_ms_{0};
    std::thread frame_;
    std::mutex attempts_m_;
    std::vector<u64> attempt_ms_;
};

/// Both engines are initialised, the host hosts a network (max 4 nodes), the client binds channel 1.
bool StartSession(Supervised& host, Supervised& client, u32* client_bind) {
    if (host.engine->Initialize(MakeNode(0xAAAA, "Host", 0), kTestSharedMemory, kTestVersion, &host.status_event) != 0 ||
        client.engine->Initialize(MakeNode(0xBBBB, "Client", 0), kTestSharedMemory, kTestVersion, &client.status_event) != 0) {
        return false;
    }
    NetworkInfo ni = MakeNetworkInfo(4, 4);
    return host.engine->BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni)) == 0 &&
           client.engine->Bind(2, 0x5F4, 1, kBroadcastNetworkNodeId, client_bind) == 0;
}

/// The client scans until the host's beacon arrives and joins its network.
bool JoinHostsNetwork(Supervised& client) {
    std::vector<u8> scan(0x4000);
    NetworkInfo net;
    if (!WaitFor([&] {
            client.engine->RecvBeaconBroadcastData(scan.data(), static_cast<u32>(scan.size()), kBroadcastMac);
            return FirstScannedNetwork(scan, &net);
        }, 8000)) {
        return false;
    }
    return client.engine->ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1) == 0;
}

double SecondsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

const Mac kHostMac = {0x00, 0x1F, 0x32, 0x44, 0x00, 0x01};
const Mac kClientMac = {0x00, 0x1F, 0x32, 0x44, 0x00, 0x02};

} // namespace

// The access point goes away and comes back (spec R-1, R-2): ENet gives up on the room after some seconds, the game's
// session ends exactly once (the engine drops to idle with reason 5, its status event and its bind event are
// signalled once), and when the link is back the console joins the room again as the same member.
static void TestLinkLoss(const std::string& host, u16 port) {
    std::printf("link loss over the real room: the access point goes away and comes back\n");
    g_rules.Reset();
    UdpGate gate(host, port);
    CHECK(gate.port() != 0);
    Supervised h(host, port, "LossHost01", "3ds-loss-host", kHostMac);
    Supervised c("127.0.0.1", gate.port(), "LossClient1", "3ds-loss-client", kClientMac);
    h.Start();
    c.Start();
    CHECK(WaitFor([&] { return h.room.joined() && c.room.joined(); }, 10000));
    u32 bind = 0;
    CHECK(StartSession(h, c, &bind));
    CHECK(JoinHostsNetwork(c));
    CHECK(c.engine->status() == NetworkStatus::ConnectedAsClient);
    c.engine->GetConnectionStatus();
    const u32 events = c.kernel.Signals(c.status_event), binds = c.kernel.Signals(bind);
    const u32 joins = c.joins, down = g_stats.transport_down[0];

    gate.Cut(true);
    const auto cut_at = std::chrono::steady_clock::now();
    CHECK(WaitFor([&] { return c.drops.load() >= 1; }, 30000));
    const double seconds = SecondsSince(cut_at);
    std::printf("  the link was cut; the game's session ended after %.1f s\n", seconds);
    CHECK(seconds >= 2.0 && seconds <= 20.0);
    const ConnectionStatus st = c.engine->GetConnectionStatus();
    CHECK_EQ(st.status, 3u);
    CHECK_EQ(st.status_change_reason, 5u); // CONNECTION_LOST
    CHECK_EQ(c.kernel.Signals(c.status_event), events + 1);
    CHECK_EQ(c.kernel.Signals(bind), binds + 1);
    CHECK_EQ(c.lefts.load(), 1u);       // one state change, one drop, no matter how the attempts to join go
    CHECK_EQ(c.drop_calls.load(), 1u);
    CHECK_EQ(c.drops.load(), 1u);
    CHECK_EQ(c.deferred.load(), 0u);    // (the thread ran fine: this is a real loss)
    CHECK_EQ(g_stats.transport_down[0] - down, 1u);
    // the host notices that the member left the room and takes its node out (the room list + 3 s)
    CHECK(WaitFor([&] { return h.engine->GetConnectionStatus().total_nodes == 1; }, 15000));

    // the access point is back: the same member joins again, nobody changes who they are
    gate.Cut(false);
    CHECK(WaitFor([&] { return c.room.joined() && c.joins.load() > joins; }, 45000));
    CHECK(c.rejoin.prefer_real_mac() && !c.rejoin.nickname_suffixed() && !c.rejoin.gave_up());
    CHECK(c.room.mac() == kClientMac);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK_EQ(c.drop_calls.load(), 1u);  // (still the only one)
    CHECK(c.engine->initialized() && c.engine->status() == NetworkStatus::NotConnected);
    // the game joins the network again; the bind it made before survived and takes the host's next frame
    CHECK(JoinHostsNetwork(c));
    CHECK(c.engine->status() == NetworkStatus::ConnectedAsClient);
    const u8 frame[] = "after the loss";
    CHECK_EQ(h.engine->SendTo(0xFFFF, 1, sizeof(frame), 0, frame, sizeof(frame)), 0u);
    std::vector<u8> out;
    u32 size = 0;
    u16 src = 0;
    CHECK(WaitFor([&] {
        CHECK_EQ(PullInto(*c.engine, 2, &out, &size, &src), 0u);
        return size != 0;
    }));
    CHECK_EQ(size, u32(sizeof(frame)));
    CHECK_RULES();
    c.Quit();
    h.Quit();
}

// The member that was lost is still listed by the room when the console comes back (spec R-2, R3): "name already in
// use" is waited out on the usual schedule, with the same name and MAC, until the room lets the old link go.
static void TestOldLinkStillListed(const std::string& host, u16 port) {
    std::printf("the room still lists the old link: the console waits for it and joins as the same member\n");
    g_rules.Reset();
    UdpGate gate(host, port);
    Supervised old_link("127.0.0.1", gate.port(), "GhostMember1", "3ds-ghost", kClientMac);
    old_link.Start();
    CHECK(WaitFor([&] { return old_link.room.joined(); }, 10000));
    gate.Cut(true); // the room hears nothing from it from now on, and no goodbye either
    old_link.Quit();
    old_link.room.RequestStop(true);

    Supervised again(host, port, "GhostMember1", "3ds-ghost", kClientMac);
    again.SetLaunchLongAgo();           // (no start-up grace: this is a console that was in the room before)
    again.rejoin.NoteLoss(platform::NowMs());
    const auto began = std::chrono::steady_clock::now();
    again.Start();
    CHECK(WaitFor([&] { return again.room.joined(); }, 45000));
    std::printf("  joined again %.1f s after the loss, after %u answer(s) 'already in the room'\n", SecondsSince(began),
                again.collisions.load());
    CHECK(SecondsSince(began) < 35.0);
    CHECK(again.collisions.load() >= 1);          // the old link was listed
    {
        // each answer "already in the room" is followed by the next attempt 2 s after the last one began, always (0.1.15: 2, 4, 6 s)
        const std::vector<u64> times = again.AttemptTimes();
        CHECK(times.size() >= 2);
        for (std::size_t i = 1; i < times.size(); ++i) {
            CHECK(times[i] - times[i - 1] >= RejoinPolicy::kRetryMs - 10 && times[i] - times[i - 1] <= RejoinPolicy::kRetryMs + 400);
        }
    }
    CHECK(again.rejoin.prefer_real_mac() && !again.rejoin.nickname_suffixed() && !again.rejoin.gave_up());
    CHECK(again.room.mac() == kClientMac);
    again.Quit();
}

// A pause of our own network thread (the framework's menu open, a loading screen starving the core) makes ENet and the
// room give up on us (spec C1): the room drops a silent member after about 5 s, the member itself notices 5 s after it
// runs again. A pause that ENet survives is nothing; one it does not survive is not a link loss either as long as the
// room takes the same member back, and the game's session goes on; beyond 10 s the loss is declared, once.
static void TestOwnPause(const std::string& host, u16 port) {
    std::printf("a pause of our own network thread: up to 8 s keeps the game's session, 12 s ends it once\n");
    g_rules.Reset();
    Supervised h(host, port, "PauseHost01", "3ds-pause-host", kHostMac);
    h.Start();
    CHECK(WaitFor([&] { return h.room.joined(); }, 10000));
    u32 bind = 0;
    for (const u32 hold : {3000u, 6000u, 8000u, 12000u}) {
        // (a new client every time: what an earlier pause left in ENet's counters is not part of this one)
        Supervised c(host, port, "PauseClient", "3ds-pause-client", kClientMac);
        c.Start();
        CHECK(WaitFor([&] { return c.room.joined(); }, 10000));
        if (hold == 3000) {
            CHECK(StartSession(h, c, &bind));
        } else {
            CHECK_EQ(c.engine->Initialize(MakeNode(0xBBBB, "Client", 0), kTestSharedMemory, kTestVersion, &c.status_event), 0u);
            CHECK_EQ(c.engine->Bind(2, 0x5F4, 1, kBroadcastNetworkNodeId, &bind), 0u);
        }
        CHECK(JoinHostsNetwork(c));
        CHECK(WaitFor([&] { return h.engine->GetConnectionStatus().total_nodes == 2; }));
        CHECK(c.engine->status() == NetworkStatus::ConnectedAsClient);
        const u32 stalls = g_stats.net_stalls;
        c.HoldNetworkThread(hold);
        CHECK(WaitFor([&] { return c.in_hold.load(); }, 3000));
        CHECK(WaitFor([&] { return !c.in_hold.load(); }, hold + 5000));
        if (hold <= 6000) {
            // Under 5 s ENet holds; around 6 s the room may or may not have dropped us by the time we run again (it
            // waits 5 s for an acknowledgement). Either way the session goes on, and if ENet did give up the room took
            // the same member back.
            std::this_thread::sleep_for(std::chrono::milliseconds(9000));
            CHECK_EQ(c.drops.load(), 0u);
            CHECK(c.engine->status() == NetworkStatus::ConnectedAsClient);
            CHECK(c.room.joined());
            const bool gave_up = c.lefts.load() == 1;
            CHECK(gave_up || c.lefts.load() == 0);
            if (gave_up) {
                CHECK_EQ(c.deferred.load(), 1u);
                CHECK_EQ(c.resumed.load(), 1u);
            }
            std::printf("  %lu s away: ENet %s\n", static_cast<unsigned long>(hold / 1000),
                        gave_up ? "gave up, the session waited and went on" : "held");
        } else if (hold == 8000) {
            // ENet (and the room) have given up on us for sure: the room takes the same member back, the session goes on
            CHECK(WaitFor([&] { return c.joins.load() > 1; }, 40000));
            CHECK_EQ(c.lefts.load(), 1u);
            CHECK_EQ(c.deferred.load(), 1u);
            CHECK_EQ(c.resumed.load(), 1u);
            CHECK_EQ(c.drops.load(), 0u);
            CHECK(c.last_stall_ms.load() >= 7500);
            CHECK(c.engine->status() == NetworkStatus::ConnectedAsClient);
            CHECK(c.rejoin.prefer_real_mac() && !c.rejoin.nickname_suffixed());
        } else {
            // 12 s: the other players have given up on us long ago: the loss is declared, once
            CHECK(WaitFor([&] { return c.drops.load() >= 1; }, 40000));
            CHECK_EQ(c.drops.load(), 1u);
            CHECK_EQ(c.deferred.load(), 0u);
            CHECK(c.engine->status() == NetworkStatus::NotConnected);
            CHECK_EQ(c.engine->GetConnectionStatus().status_change_reason, 5u);
            CHECK(WaitFor([&] { return c.room.joined() && c.joins.load() > 1; }, 45000));
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            CHECK_EQ(c.drops.load(), 1u);
        }
        CHECK(g_stats.net_stalls >= stalls + 1);
        c.Quit();
        c.room.Stop();
        CHECK(WaitFor([&] { return h.engine->GetConnectionStatus().total_nodes == 1; }, 12000));
    }
    CHECK_RULES();
    h.Quit();
}

// A player who leaves the room takes its node along (spec R-4, F15), and a goodbye queued just before the network
// thread stops is sent (spec R7).
static void TestLeavingTheRoom(const std::string& host, u16 port) {
    std::printf("a player leaves the room: the host's node goes with it; a queued goodbye is sent before the quick stop\n");
    g_rules.Reset();
    Supervised h(host, port, "LeaveHost01", "3ds-leave-host", kHostMac);
    h.Start();
    CHECK(WaitFor([&] { return h.room.joined(); }, 10000));
    u32 bind = 0;
    for (const int how : {0, 1, 2}) {
        // 0: a polite leave, 1: the quick stop of an exiting game, 2: the game's Shutdown while the network thread is
        // busy (its Deauthentication is still queued when the stop comes)
        Supervised c(host, port, "LeaveClient1", "3ds-leave-client", kClientMac);
        c.Start();
        CHECK(WaitFor([&] { return c.room.joined(); }, 10000));
        if (how == 0) {
            CHECK(StartSession(h, c, &bind));
        } else {
            u32 ignored = 0;
            // (the host keeps its network from the first round; the client is a new engine every time)
            CHECK_EQ(c.engine->Initialize(MakeNode(0xBBBB, "Client", 0), kTestSharedMemory, kTestVersion, &c.status_event), 0u);
            CHECK_EQ(c.engine->Bind(2, 0x5F4, 1, kBroadcastNetworkNodeId, &ignored), 0u);
        }
        CHECK(JoinHostsNetwork(c));
        CHECK(WaitFor([&] { return h.engine->GetConnectionStatus().total_nodes == 2; }));
        h.engine->GetConnectionStatus();
        const u32 by_presence = g_stats.node_removed[3], by_deauth = g_stats.node_removed[0];
        const auto left_at = std::chrono::steady_clock::now();
        c.Quit(); // (nobody supervises it any more)
        if (how == 0) {
            c.room.Stop();
        } else if (how == 1) {
            c.room.RequestStop(true);
            c.room.StopWithin(2000);
        } else {
            c.HoldNetworkThread(1500);
            CHECK(WaitFor([&] { return c.in_hold.load(); }, 3000));
            CHECK_EQ(c.engine->Shutdown(), 0u); // queues the Deauthentication for its host
            c.room.RequestStop(true);
            CHECK(WaitFor([&] { return !c.in_hold.load(); }, 5000));
            c.room.StopWithin(2000);
        }
        CHECK(WaitFor([&] { return h.engine->GetConnectionStatus().total_nodes == 1; }, 12000));
        const double took = SecondsSince(left_at);
        std::printf("  %s: the node was gone after %.1f s\n",
                    how == 0 ? "polite leave" : how == 1 ? "quick stop" : "Shutdown, then the quick stop", took);
        if (how == 2) {
            // the goodbye got out: the host dropped the node on the Deauthentication, long before the room's list and
            // the 3 s debounce could have
            CHECK(took < 2.9);
            CHECK_EQ(g_stats.node_removed[0], by_deauth + 1);
            CHECK_EQ(g_stats.node_removed[3], by_presence);
        } else {
            CHECK(took > 2.5); // (the list of the room, and 3 s)
            CHECK_EQ(g_stats.node_removed[3], by_presence + 1);
        }
    }
    CHECK_RULES();
    h.Quit();
}

// The network thread remembers a pause of 2 s or more for as long as a loss can still be put down to it (spec C1).
static void TestPauseIsRemembered() {
    std::printf("a pause of the network thread of 2 s or more is remembered\n");
    StubRoom server;
    TxClient client(server, false);
    CHECK(client.Join());
    CHECK(WaitFor([&] { return client.room.queued_frames() == 0; }));
    CHECK_EQ(client.room.RecentStallMs(platform::NowMs(), 35000), 0u);
    CHECK(client.Hold());
    std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // (a short pause is nothing)
    client.Release();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_EQ(client.room.RecentStallMs(platform::NowMs(), 35000), 0u);
    CHECK(client.Hold());
    std::this_thread::sleep_for(std::chrono::milliseconds(2300));
    client.Release();
    CHECK(WaitFor([&] { return client.room.RecentStallMs(platform::NowMs(), 35000) != 0; }));
    const u32 stall = client.room.RecentStallMs(platform::NowMs(), 35000);
    CHECK(stall >= 2300 && stall < 3500);
    // it is forgotten once it is older than the look-back asked for
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK_EQ(client.room.RecentStallMs(platform::NowMs(), 200), 0u);
    CHECK_EQ(client.room.RecentStallMs(platform::NowMs(), 5000), stall);
}

// The quick stop of an exiting game or a sleeping console (spec R7) sends what the game queued just before it, a
// Deauthentication among it: the disconnect that follows throws away whatever ENet still holds.
static void TestQuickStopSendsWhatIsQueued() {
    std::printf("quick stop: the frames queued before it go out first\n");
    StubRoom server;
    TxClient client(server, false);
    CHECK(client.Join());
    CHECK(WaitFor([&] { return client.room.queued_frames() == 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server.Forget();
    CHECK(client.Hold());
    for (u16 seq = 0; seq < 5; ++seq) {
        client.room.SendWifiPacket(TxFrame(seq, 100));
    }
    client.room.SendWifiPacket(TxDeauth());
    client.room.RequestStop(true);
    client.Release();
    CHECK(client.room.StopWithin(3000));
    CHECK(WaitFor([&] { return server.Count() >= 6; }, 3000));
    const auto got = server.Frames();
    int data = 0, deauth = 0;
    for (const auto& g : got) {
        data += g.type == 1;
        deauth += g.type == 4;
    }
    CHECK_EQ(data, 5);
    CHECK_EQ(deauth, 1);
}

// The wait for the goodbye's acknowledgement (120 ms) has a deadline: a room that keeps sending while it
// acknowledges nothing must not keep the stopping thread in it (the plugin gives that thread 400 ms at exit).
static void TestQuickStopWaitIsBounded() {
    std::printf("quick stop: the wait for the goodbye ends on time although the room keeps sending\n");
    StubRoom server;
    TxClient client(server, false);
    CHECK(client.Join());
    CHECK(WaitFor([&] { return client.room.queued_frames() == 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(client.Hold());
    server.Flood(true); // reads nothing, so the Deauthentication below is never acknowledged
    CHECK(WaitFor([&] { return server.FloodStarted(); }));
    client.room.SendWifiPacket(TxDeauth());
    client.room.RequestStop(true);
    const u64 started = platform::NowMs();
    client.Release();
    const bool stopped = client.room.StopWithin(3000);
    const u64 took = platform::NowMs() - started;
    server.Flood(false); // (before the checks: a wait that never ends must not hang the test)
    CHECK(stopped);
    CHECK(took >= 100);  // it did wait for the acknowledgement that does not come ...
    CHECK(took < 1000);  // ... and gave up on time (120 ms plus the disconnect)
}

// What the room itself says (spec R2): a closed room, a kick and a ban end the game's session at once, and a closed
// room is told apart from a lost link.
static void TestRoomSaysGoodbye() {
    std::printf("the room closes, kicks, bans: the session ends at once and the error says why\n");
    struct Case {
        u8 message;
        RoomClient::Error error;
        bool joins_again;
    };
    for (const Case& test : {Case{room::IdCloseRoom, RoomClient::Error::RoomClosed, true},
                             Case{room::IdHostKicked, RoomClient::Error::HostKicked, false},
                             Case{room::IdHostBanned, RoomClient::Error::HostBanned, false}}) {
        StubRoom server;
        CHECK(server.port() != 0);
        Supervised c("127.0.0.1", server.port(), "RoomEnds001", "3ds-room-ends", kClientMac);
        c.Start();
        CHECK(WaitFor([&] { return c.room.joined(); }));
        u32 ignored = 0;
        CHECK_EQ(c.engine->Initialize(MakeNode(0xBBBB, "Client", 0), kTestSharedMemory, kTestVersion, &c.status_event), 0u);
        CHECK_EQ(c.engine->Bind(2, 0x5F4, 1, kBroadcastNetworkNodeId, &ignored), 0u);
        server.SendToClient({test.message});
        CHECK(WaitFor([&] { return c.lefts.load() >= 1; }));
        CHECK_EQ(c.last_error.load(), static_cast<int>(test.error));
        CHECK_EQ(c.drop_calls.load(), 1u);   // at once: no pause of the thread, no waiting
        CHECK_EQ(c.deferred.load(), 0u);
        if (test.joins_again) {
            CHECK(WaitFor([&] { return c.joins.load() >= 2; }, 15000)); // a room that is open again is joined again
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            CHECK_EQ(c.joins.load(), 1u);    // the user has to look at this one
            CHECK(c.rejoin.gave_up());
        }
        c.Quit();
    }
    CHECK_EQ(std::string(RoomClient::ErrorName(RoomClient::Error::RoomClosed)), std::string("Room closed by its host"));
}

// L1: every failed attempt says why, with the errno of the socket call that failed (0.1.15 logged "enet_host_create failed (no
// socket)" and nothing else). Here: a room that is not there - the socket works, nothing answers, ENet gives up after 5 s.
static void TestFailedAttemptIsLogged() {
    std::printf("a failed attempt to join says why: the log has the reason and the errno\n");
    LogOn log_on;
    std::mutex m;
    std::vector<std::string> lines;
    SetLogSink([&](const char* line) {
        std::lock_guard<std::mutex> l(m);
        lines.push_back(line);
    });
    RoomClient room;
    RoomClient::Config config;
    config.host = "127.0.0.1";
    config.port = 1; // nobody listens there
    config.nickname = "NoRoomHere01";
    config.console_id_hash = "3ds-noroom";
    config.game_name = "Test Game";
    config.game_id = 0x0004000000164800ULL;
    CHECK(room.Start(config));
    CHECK(WaitFor([&] { return room.state() == RoomClient::State::Idle && room.error() != RoomClient::Error::None; }, 9000));
    CHECK(room.error() == RoomClient::Error::CouldNotConnect);
    room.Stop();
    SetLogSink([](const char*) {});
    bool said = false;
    std::lock_guard<std::mutex> l(m);
    for (const std::string& line : lines) {
        said = said || line.rfind("net: the room did not answer within 5000 ms (last socket error while waiting: errno ", 0) == 0;
    }
    CHECK(said);
    // C7: the name the attempt used and what it came to, with the time the resolver took (a dotted quad needs none)
    bool resolved = false;
    for (const std::string& line : lines) {
        resolved = resolved || line.rfind("net: '127.0.0.1' resolves to 127.0.0.1 in ", 0) == 0;
    }
    CHECK(resolved);
}

// C7: the log says what a host name came to and how long the resolver took, and when it could not resolve it
static void TestResolutionIsLogged() {
    std::printf("the log says what the room's name resolves to, and how long that took\n");
    LogOn log_on;
    std::mutex m;
    std::vector<std::string> lines;
    SetLogSink([&](const char* line) {
        std::lock_guard<std::mutex> l(m);
        lines.push_back(line);
    });
    RoomClient::Config config;
    config.port = 1; // (the room is not there: only the lookup matters)
    config.nickname = "NoRoomHere02";
    config.console_id_hash = "3ds-noroom2";
    config.game_name = "Test Game";
    config.game_id = 0x0004000000164800ULL;
    {
        RoomClient room;
        config.host = "localhost";
        CHECK(room.Start(config));
        CHECK(WaitFor([&] {
            std::lock_guard<std::mutex> l(m);
            for (const std::string& line : lines) {
                if (line.rfind("net: 'localhost' resolves to ", 0) == 0) {
                    return true;
                }
            }
            return false;
        }, 5000));
        room.Stop();
    }
    {
        RoomClient room;
        config.host = "no-such-host.invalid";
        CHECK(room.Start(config));
        CHECK(WaitFor([&] { return room.state() == RoomClient::State::Idle && room.error() != RoomClient::Error::None; }, 20000));
        room.Stop();
    }
    SetLogSink([](const char*) {});
    std::lock_guard<std::mutex> l(m);
    bool localhost = false, failed = false;
    for (const std::string& line : lines) {
        const std::string head = "net: 'localhost' resolves to ";
        if (line.rfind(head, 0) == 0) {
            // "... resolves to 127.0.0.1 in 3 ms"
            unsigned a = 0, b = 0, c = 0, d = 0;
            unsigned long ms = 99999;
            localhost = std::sscanf(line.c_str() + head.size(), "%u.%u.%u.%u in %lu ms", &a, &b, &c, &d, &ms) == 5 && a == 127 && ms < 5000;
        }
        failed = failed || (line.rfind("net: could not resolve 'no-such-host.invalid', errno ", 0) == 0 && line.find(", after ") != std::string::npos);
    }
    CHECK(localhost);
    CHECK(failed);
}

// L2: a socket that fails (the console woke up from a sleep with no WiFi yet: ENETDOWN on every call) must not turn the network
// loop into a spin. 0.1.15 paused 2 ms after each failing enet_host_service() call: 680 of them in 1.8 s on hardware. The
// pause now grows with the errors in a row (2, 4 ... 128 ms), the errno is kept, and the link is picked up again when the
// socket works. The failure is made by putting /dev/null under the client's socket (every call on it fails) and the socket
// back afterwards.
static void TestSocketErrorsAreSpacedOut() {
    std::printf("socket errors: the pause grows with the errors in a row, the errno is kept, the link comes back\n");
    // the schedule
    CHECK_EQ(RoomClient::ServiceErrorPauseMs(0), 0u);
    const u32 expected[] = {2, 4, 8, 16, 32, 64, 128, 128, 128};
    for (u32 i = 0; i < 9; ++i) {
        CHECK_EQ(RoomClient::ServiceErrorPauseMs(i + 1), expected[i]);
    }
    u32 slept = 0, calls = 0;
    while (slept < 1800) {
        slept += RoomClient::ServiceErrorPauseMs(++calls);
    }
    CHECK(calls <= 20); // 1.8 s of failing calls: 19 (0.1.15: 680)

    StubRoom server;
    TxClient client(server, false);
    CHECK(client.Join());
    CHECK(WaitFor([&] { return client.room.queued_frames() == 0; }));
    CHECK_EQ(client.room.last_errno(), 0);
    // the client's socket: the one datagram socket of this process that is not the stub's
    int fd = -1, candidates = 0;
    for (int f = 3; f < 256; ++f) {
        struct stat st;
        int type = 0;
        socklen_t length = sizeof(type);
        if (f != server.socket() && fstat(f, &st) == 0 && S_ISSOCK(st.st_mode) &&
            getsockopt(f, SOL_SOCKET, SO_TYPE, &type, &length) == 0 && type == SOCK_DGRAM) {
            fd = f;
            ++candidates;
        }
    }
    CHECK_EQ(candidates, 1);
    if (candidates != 1) {
        return;
    }
    const int devnull = open("/dev/null", O_RDWR);
    const int saved = dup(fd);
    (void)TakeStats();                                   // (a window of the statistics that starts here)
    const u32 errors_before = g_stats.service_errors;
    const u64 window_began = platform::NowTicks();
    dup2(devnull, fd);                                   // the socket is gone from under the client
    std::this_thread::sleep_for(std::chrono::milliseconds(1800));
    const u32 failed = g_stats.service_errors - errors_before;
    std::printf("  1.8 s of a failing socket: %u failing calls\n", failed);
    CHECK(failed >= 5 && failed <= 29);
    {
        // The pauses between the failing calls are the loop waiting, not working: they are counted apart and are not in the
        // CPU of the network thread (0.1.15 counted a pause as queue+tick, so a failing socket looked like a busy plugin).
        const u64 window_ms = azl_ticks_to_ms(platform::NowTicks() - window_began);
        const Stats window = TakeStats();
        const CpuNumbers c = MakeCpuNumbers(window, window_ms);
        std::printf("  the same window: loop %lu.%lu ms/s = cpu %lu.%lu + pauses %lu.%lu + ...\n", static_cast<unsigned long>(c.wall10 / 10),
                    static_cast<unsigned long>(c.wall10 % 10), static_cast<unsigned long>(c.net10 / 10),
                    static_cast<unsigned long>(c.net10 % 10), static_cast<unsigned long>(c.pause10 / 10),
                    static_cast<unsigned long>(c.pause10 % 10));
        CHECK(c.wall10 >= 9000 && c.wall10 <= 10200);   // the loop ran the whole window (1000 ms per second)
        CHECK(c.pause10 >= 7000);                       // and spent nearly all of it in the pauses
        CHECK(c.net10 <= 500);                          // ... which are not CPU: under 50 ms per second
        CHECK(c.net10 + c.pause10 + c.soc10 + c.poll10 + c.netlock10 <= c.wall10 + 300); // nothing counted twice
        CHECK(window.t_net_pause > 0);
    }
    CHECK(client.room.last_errno() != 0);                // ENOTSOCK here, ENETDOWN on the console
    CHECK(client.room.joined());                         // (ENet gives up after 5 s, not before)
    dup2(saved, fd);                                     // the socket works again
    CHECK(WaitFor([&] {
        const u32 now = g_stats.service_errors;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return g_stats.service_errors == now;
    }, 3000));
    close(saved);
    close(devnull);
    // and the link carries frames again
    server.Forget();
    client.room.SendWifiPacket(TxFrame(7, 100));
    CHECK(WaitFor([&] { return server.Count() >= 1; }, 3000));
    CHECK(client.room.joined());
}

// The CPU of the network thread (U3): the sections of its loop are cut at the clock reads it already makes and the waits that
// fell in them - the soc calls, the engine's lock, a pause - are taken off, so what is left is CPU and the pieces add up to
// the wall time of the loop. Over a joined link with a little traffic. In the build with the plugin's own ENet platform layer
// (Sources/enet/enet_3ds.c) the soc calls are timed, and an idle loop is nearly all poll() wait with a few percent of CPU; with
// ENet's unix.c nothing times the soc calls, so only the wall time and the bound of "nothing counted twice" are checked.
static void TestCpuAccounting() {
    std::printf("CPU accounting of the network loop: its CPU and its waits add up to its wall time\n");
    StubRoom server;
    TxClient client(server, false);
    CHECK(client.Join());
    CHECK(WaitFor([&] { return client.room.queued_frames() == 0; }));
    (void)TakeStats();
    const u64 began = platform::NowTicks();
    for (int i = 0; i < 60; ++i) { // 30 frames a second for 2 s
        client.room.SendWifiPacket(TxFrame(7, 100));
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    const u64 window_ms = azl_ticks_to_ms(platform::NowTicks() - began);
    const Stats s = TakeStats();
    const CpuNumbers c = MakeCpuNumbers(s, window_ms);
    const unsigned long accounted = c.net10 + c.soc10 + c.poll10 + c.pause10 + c.netlock10;
    std::printf("  window %lu ms: loop %lu.%lu ms/s = cpu %lu.%lu + soc %lu.%lu + poll %lu.%lu + pauses %lu.%lu + other %lu.%lu"
                " (soc calls timed: %lu)\n",
                static_cast<unsigned long>(window_ms), static_cast<unsigned long>(c.wall10 / 10),
                static_cast<unsigned long>(c.wall10 % 10), static_cast<unsigned long>(c.net10 / 10),
                static_cast<unsigned long>(c.net10 % 10), static_cast<unsigned long>(c.soc10 / 10),
                static_cast<unsigned long>(c.soc10 % 10), static_cast<unsigned long>(c.poll10 / 10),
                static_cast<unsigned long>(c.poll10 % 10), static_cast<unsigned long>(c.pause10 / 10),
                static_cast<unsigned long>(c.pause10 % 10), static_cast<unsigned long>(c.other10 / 10),
                static_cast<unsigned long>(c.other10 % 10),
                static_cast<unsigned long>(s.soc_calls[kSocPoll] + s.soc_calls[kSocRecvData] + s.soc_calls[kSocSend]));
    CHECK(c.wall10 >= 9500 && c.wall10 <= 10200); // the loop ran the whole window
    CHECK(c.net10 <= c.wall10);
    CHECK(accounted <= c.wall10 + 300);           // nothing is counted twice (30 ms per second of slack for the clock reads)
    CHECK(s.t_net_pause == 0);                    // (a socket that works: no pause)
    CHECK(c.plugin10 == c.net10 + c.uds10 + c.frame10 + c.writer10);
#ifdef AZL_E3DS
    CHECK(s.soc_calls[kSocPoll] > 0); // (this build runs enet_3ds.c: it must have timed the calls of the network thread)
#endif
    if (s.soc_calls[kSocPoll] > 0) {
        // the plugin's platform layer timed every call: the loop's wall time is explained by its CPU and its waits, the CPU is
        // a small part (the PC does 30 frames a second in a few ms), and the poll() wait is the bulk
        CHECK(c.poll10 >= 5000);
        CHECK(c.net10 <= 1500);                   // < 150 ms per second of CPU (ASAN included)
        CHECK(c.other10 <= 500);                  // < 50 ms per second of wall time that neither CPU nor a wait explains
        CHECK(s.soc_calls[kSocSend] >= 30);       // the frames went out through the timed send
        CHECK(s.soc_ticks[kSocSend] > 0);
        CHECK(s.soc_ticks[kSocPoll] >= s.soc_calls[kSocPoll] * 1000ull);   // (a poll takes longer than a microsecond)
    }
    client.room.Stop();
}

// L3: stopping the client waits for its network thread, but not for ever: one that is stuck (in a service call, or not
// scheduled) is abandoned after RoomClient::kStopJoinMs and told that its session is over. It must not touch the next one: not
// its state, not its queue, no goodbye on a connection that is not the client's any more.
static void TestStuckNetworkThreadIsAbandoned() {
    std::printf("a stuck network thread is abandoned after 2 s and cannot disturb the next session\n");
    StubRoom server;
    RoomClient room;
    RoomClient::Config config;
    config.host = "127.0.0.1";
    config.port = server.port();
    config.nickname = "StuckThread1";
    config.console_id_hash = "3ds-stuck";
    config.game_name = "Test Game";
    config.game_id = 0x0004000000164800ULL;
    std::atomic<u32> hold_ms{0};
    std::atomic<bool> in_hold{false}, hold_over{false};
    std::atomic<u32> idles{0}, joined_calls{0};
    std::atomic<std::size_t> old_thread{0};  // the thread that sits in the callback (as a hash of its id)
    std::atomic<u32> old_thread_ticks{0};    // calls of the callback from that thread once it is awake again
    room.SetTickCallback([&](u64) {
        const std::size_t me = std::hash<std::thread::id>()(std::this_thread::get_id());
        if (hold_over.load() && me == old_thread.load()) {
            ++old_thread_ticks;
        }
        if (const u32 ms = hold_ms.exchange(0)) {
            old_thread = me;
            in_hold = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            in_hold = false;
            hold_over = true;
        }
    });
    room.SetStateCallback([&](RoomClient::State state, RoomClient::Error) {
        idles += state == RoomClient::State::Idle;
        joined_calls += state == RoomClient::State::Joined;
    });
    CHECK(room.Start(config));
    CHECK(WaitFor([&] { return room.joined(); }));
    hold_ms = 5000;
    CHECK(WaitFor([&] { return in_hold.load(); }));

    const auto began = std::chrono::steady_clock::now();
    room.Stop();                                         // the thread is asleep in the tick callback for 5 s
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    std::printf("  Stop() returned after %.2f s (the thread sleeps 5 s)\n", took);
    CHECK(took >= 1.9 && took < 3.5);
    CHECK(room.state() == RoomClient::State::Idle);

    // the next session starts at once, with the old thread still asleep
    CHECK(room.Start(config));
    CHECK(WaitFor([&] { return room.joined(); }));
    CHECK_EQ(server.Connects(), 2u);
    const u32 idles_then = idles.load();
    const u32 joins_then = joined_calls.load();
    CHECK(WaitFor([&] { return hold_over.load(); }, 8000)); // the old thread wakes up ...
    std::this_thread::sleep_for(std::chrono::milliseconds(800)); // ... and leaves
    CHECK(room.joined());                                 // the new session is untouched: not stopped, not Idle
    CHECK_EQ(old_thread_ticks.load(), 0u);                // the old thread went back to its owner: no round, no tick more
    CHECK_EQ(idles.load(), idles_then);
    CHECK_EQ(joined_calls.load(), joins_then);
    CHECK_EQ(server.Connects(), 2u);                      // (nothing opened a connection of its own)
    server.Forget();
    room.SendWifiPacket(TxFrame(3, 100));                 // the new session's queue is the new thread's
    CHECK(WaitFor([&] { return server.Count() >= 1; }, 3000));
    CHECK_EQ(server.Count(), std::size_t(1));
    room.Stop();
    CHECK(room.state() == RoomClient::State::Idle);
}

// L3, the other places a thread gets stuck: in the game's frame handler (the wifi callback), where the loop is between its two
// checks of the session. The thread that Stop() gave up on wakes there, 4.5 s later, with the next session already running and its
// queue full of that session's frames: it must go back to its owner without taking one (the room would get them from a
// connection that is not the client's any more, one more member under the old identity) and without touching the state.
static void TestAbandonedThreadStuckInTheFrameHandler() {
    std::printf("an abandoned thread that was stuck in the frame handler takes nothing of the next session\n");
    StubRoom server;
    RoomClient room;
    RoomClient::Config config;
    config.host = "127.0.0.1";
    config.port = server.port();
    config.nickname = "StuckHandler1";
    config.console_id_hash = "3ds-stuck-handler";
    config.game_name = "Test Game";
    config.game_id = 0x0004000000164800ULL;
    std::atomic<bool> in_hold{false}, hold_over{false}, armed{true};
    std::atomic<u32> states{0};
    room.SetWifiCallback([&](const WifiPacket&) {
        if (armed.exchange(false)) {
            in_hold = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(4500));
            in_hold = false;
            hold_over = true;
        }
    });
    room.SetStateCallback([&](RoomClient::State, RoomClient::Error) { ++states; });
    CHECK(room.Start(config));
    CHECK(WaitFor([&] { return room.joined(); }));
    server.SendToClient(room::EncodeWifiPacket(TxFrame(1, 100)));      // the frame the thread is held in
    CHECK(WaitFor([&] { return in_hold.load(); }));
    room.Stop();                                                       // gives up on it after 2 s
    CHECK(room.Start(config));
    CHECK(WaitFor([&] { return room.joined(); }));
    CHECK_EQ(server.Connects(), 2u);
    server.Forget();
    // the new session's queue is never empty until the old thread has woken up and left
    u16 seq = 1000;
    const auto began = std::chrono::steady_clock::now();
    while (!hold_over.load() || std::chrono::steady_clock::now() - began < std::chrono::milliseconds(400)) {
        room.SendWifiPacket(TxFrame(seq++, 60));
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        if (std::chrono::steady_clock::now() - began > std::chrono::seconds(10)) {
            break;
        }
    }
    CHECK(hold_over.load());
    const u32 states_then = states.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    std::set<const void*> peers;
    for (const StubRoom::Got& g : server.Frames()) {
        if (g.type == static_cast<u8>(WifiPacket::Type::Data)) {
            peers.insert(g.peer);
        }
    }
    std::printf("  %u frames sent in the new session; the room got them from %zu connection(s)\n",
                static_cast<unsigned>(seq - 1000), peers.size());
    CHECK_EQ(peers.size(), std::size_t(1)); // all through the new session's connection, none through the old one's
    CHECK(room.joined());
    CHECK_EQ(states.load(), states_then);   // the old thread said nothing about the client's state either
    CHECK_EQ(server.Connects(), 2u);
    room.Stop();
}

// L1 over the real room: the access point goes away and comes back, and the WiFi byte of the shared page says so. While it
// is down no attempt to join is made at all (0.1.15 tried on its 2, 4, 6, 8, 10 s ladder: 16 attempts of 17 failed), the
// moment it is back an attempt is made (within one look at the byte, 500 ms), and a room that refuses for good (a wrong
// password) is not tried again, WiFi or not.
static void TestWifiGate(const std::string& host, u16 port, const std::string& host2, u16 port2) {
    std::printf("WiFi gate over the real room: no attempt while it is down, one at once when it is back, none after a refusal\n");
    g_rules.Reset();
    // (MACs of their own: a ghost of an earlier test that the room still lists would turn the first attempt into two)
    const Mac mac1 = {0x00, 0x1F, 0x32, 0x44, 0x00, 0x11}, mac2 = {0x00, 0x1F, 0x32, 0x44, 0x00, 0x12},
              mac3 = {0x00, 0x1F, 0x32, 0x44, 0x00, 0x13};
    {
        UdpGate gate(host, port);
        CHECK(gate.port() != 0);
        Supervised c("127.0.0.1", gate.port(), "WifiGate001", "3ds-wifi-gate", mac1);
        c.Start();
        CHECK(WaitFor([&] { return c.room.joined(); }, 10000));
        CHECK_EQ(c.attempts.load(), 1u);
        CHECK(!c.rejoin.waiting_for_wifi());

        // the access point goes away: the byte says "not connected", nothing gets through
        c.wifi_state = 1;
        gate.Cut(true);
        const u32 attempts_at_cut = c.attempts.load();
        CHECK(WaitFor([&] { return c.lefts.load() >= 1; }, 30000));   // ENet gives up after some seconds
        CHECK(WaitFor([&] { return c.rejoin.waiting_for_wifi(); }, 3000));
        std::this_thread::sleep_for(std::chrono::seconds(10));        // ... and the WiFi stays away
        CHECK_EQ(c.attempts.load(), attempts_at_cut);                 // not one attempt in all that time
        CHECK(!c.room.joined());

        // back
        c.wifi_state = RejoinPolicy::kWifiConnected;
        gate.Cut(false);
        const u64 back = platform::NowMs();
        CHECK(WaitFor([&] { return c.attempts.load() > attempts_at_cut; }, 3000));
        const std::vector<u64> times = c.AttemptTimes();
        const u64 first = times.back() >= back && times.size() > attempts_at_cut ? times[attempts_at_cut] : 0;
        std::printf("  the WiFi was back; the attempt came %llu ms later\n", static_cast<unsigned long long>(first - back));
        CHECK(first >= back && first - back <= RejoinPolicy::kWifiPollMs + 300);
        CHECK(WaitFor([&] { return c.room.joined(); }, 30000));
        CHECK(!c.rejoin.waiting_for_wifi());
        c.Quit();
    }
    {
        // it drops again before the join worked: the retries stop with it (a gate that stays cut makes every attempt fail
        // after the 5 s ENet waits for the room)
        UdpGate gate(host, port);
        Supervised c("127.0.0.1", gate.port(), "WifiGate002", "3ds-wifi-gate2", mac2);
        gate.Cut(true);
        c.Start();
        CHECK(WaitFor([&] { return c.attempts.load() >= 2; }, 20000)); // the WiFi byte says "connected": it keeps trying
        const std::vector<u64> times = c.AttemptTimes();
        CHECK(times.size() >= 2 && times[1] - times[0] >= RejoinPolicy::kRetryMs);
        c.wifi_state = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(RejoinPolicy::kWifiPollMs + 200));
        // (an attempt that was already running ends with its 5 s)
        CHECK(WaitFor([&] { return c.rejoin.waiting_for_wifi(); }, 12000));
        const u32 attempts_now = c.attempts.load();
        std::this_thread::sleep_for(std::chrono::seconds(6));
        CHECK_EQ(c.attempts.load(), attempts_now);
        c.Quit();
    }
    {
        // a refusal for good: the wrong password (room 2). No new attempt, with the WiFi up or going away and coming back.
        Supervised c(host2, port2, "WifiGate003", "3ds-wifi-gate3", mac3);
        c.SetPassword("wrong");
        c.Start();
        CHECK(WaitFor([&] { return c.last_error.load() == static_cast<int>(RoomClient::Error::WrongPassword); }, 10000));
        CHECK(WaitFor([&] { return c.rejoin.gave_up(); }, 2000));
        const u32 attempts_now = c.attempts.load();
        c.wifi_state = 0;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        c.wifi_state = RejoinPolicy::kWifiConnected;
        std::this_thread::sleep_for(std::chrono::seconds(4));
        CHECK_EQ(c.attempts.load(), attempts_now);
        c.Quit();
    }
    CHECK_RULES();
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--tx-queue") { // (no room server needed)
        SetLogSink([](const char*) {});
        TestTxQueueStalledServer();
        TestEnetPacketLimit();
        TestPauseIsRemembered();
        TestQuickStopSendsWhatIsQueued();
        TestQuickStopWaitIsBounded();
        TestRoomSaysGoodbye();
        TestSocketErrorsAreSpacedOut();
        TestCpuAccounting();
        TestStuckNetworkThreadIsAbandoned();
        TestAbandonedThreadStuckInTheFrameHandler();
        TestFailedAttemptIsLogged();
        TestResolutionIsLogged();
        std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return g_failures ? 1 : 0;
    }
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s room1-host room1-port room2-host room2-port | --tx-queue\n", argv[0]);
        return 2;
    }
    SetLogSink([](const char* line) { std::fprintf(stderr, "  [log] %s\n", line); });
    const std::string host1 = argv[1], host2 = argv[3];
    const u16 port1 = static_cast<u16>(std::atoi(argv[2])), port2 = static_cast<u16>(std::atoi(argv[4]));
    TestRoomBasics(host1, port1, host2, port2);
    TestUdsOverRealRoom(host1, port1);
    TestTxQueueStalledServer();
    TestEnetPacketLimit();
    TestPauseIsRemembered();
    TestQuickStopSendsWhatIsQueued();
    TestQuickStopWaitIsBounded();
    TestRoomSaysGoodbye();
    TestSocketErrorsAreSpacedOut();
    TestCpuAccounting();
    TestStuckNetworkThreadIsAbandoned();
    TestAbandonedThreadStuckInTheFrameHandler();
    TestFailedAttemptIsLogged();
    TestResolutionIsLogged();
    TestLeavingTheRoom(host1, port1);
    TestLinkLoss(host1, port1);
    TestOldLinkStillListed(host1, port1);
    TestOwnPause(host1, port1);
    TestWifiGate(host1, port1, host2, port2);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
