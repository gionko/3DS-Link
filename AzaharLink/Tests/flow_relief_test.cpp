// The relief of a dense broadcast stream (Includes/azl/flow_relief.hpp; 2026-11-14, Mario Party: Island Tour): the table of titles
// (Includes/azl/title_rules.hpp, every row),
// the same-frame filter alone and in UdsEngine::SendTo, the hand-over burst of RoomClient::Run against an ENet server in this
// process, the statistics line - and, for every other title, that nothing of it happens: every frame goes, one packet a round.
// Synthetic data only. The dead end of ENet's limit of waiting data is Tests/enet_cap_test.cpp; what the two reliefs do to the game's
// flow on a console's costs is the model's part (Tests/vsim/island_dice.sh).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <enet/enet.h>

#include "azl/flow_relief.hpp"
#include "azl/room_client.hpp"
#include "azl/room_proto.hpp"
#include "azl/stats.hpp"
#include "azl/title_rules.hpp"
#include "mock_room.hpp"
#include "support.hpp"

using namespace azl;

namespace {

template <typename F>
bool WaitFor(F&& done, int timeout_ms = 5000) {
    for (int i = 0; i < timeout_ms; ++i) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}

// ---- the table of the titles ----------------------------------------------------------------------------------------------------
// Every row of azl/title_rules.hpp, whatever it holds: a row added later is run here with no new line of this file.
void TestTitles() {
    std::printf("flow relief: the table of the titles (every row: every region, the application and its Download Play child) and no other title\n");
    CHECK(title_rules::kRowCount >= 1);
    static_assert(title_rules::RowsAreSound(), "a constant expression");
    for (std::size_t r = 0; r < title_rules::kRowCount; ++r) {
        const title_rules::Row& row = title_rules::kRows[r];
        const title_rules::Rules& want = row.rules;
        const bool relief = want.same_frame_ms != 0 || want.hand_over_burst > 1 || want.cap_drop_max != 0 || want.send_budget_bytes != 0;
        std::size_t ids = 0;
        for (const u32 low : row.low) {
            if (low == 0) {
                continue;
            }
            ++ids;
            const u64 app = 0x0004000000000000ull | low;
            const u64 child = 0x0004000100000000ull | low;
            for (const u64 id : {app, child}) {
                CHECK_EQ(title_rules::RowOf(id), static_cast<int>(r));
                const title_rules::Rules got = title_rules::RulesFor(id);
                CHECK_EQ(got.same_frame_ms, want.same_frame_ms);
                CHECK_EQ(got.hand_over_burst, want.hand_over_burst);
                CHECK_EQ(got.cap_drop_max, want.cap_drop_max);
                CHECK_EQ(got.host_keeps_to_its_game, want.host_keeps_to_its_game);
                CHECK_EQ(got.send_budget_bytes, want.send_budget_bytes);
                // the start-up rules: the application only (its child starts as it ever did)
                const title_rules::Rules app_rules = title_rules::AppRulesFor(id);
                const bool is_app = id == app;
                CHECK_EQ(app_rules.worker_off_core1, is_app && want.worker_off_core1);
                CHECK_EQ(app_rules.late_soc_block, is_app && want.late_soc_block);
                CHECK_EQ(app_rules.soc_guest, is_app && want.soc_guest);
                CHECK_EQ(flow::IsDenseTitle(id), relief);
                CHECK_EQ(std::string(title_rules::NameOf(id)), std::string(row.name));
                CHECK(title_rules::SameGame(id, app) && title_rules::SameGame(child, id));
            }
            // the Download Play column: the application's, value by value; never its child's (as the plugin's table compared whole ids)
            const title_rules::Dlp dlp = title_rules::DlpOf(app);
            CHECK(dlp.mode == row.dlp.mode);
            CHECK_EQ(dlp.console_seed, row.dlp.console_seed);
            CHECK_EQ(dlp.max_players, row.dlp.max_players);
            CHECK(title_rules::DlpOf(child).mode == DlpGameMode::None);
            CHECK(!title_rules::DlpOf(child).console_seed && title_rules::DlpOf(child).max_players == title_rules::kDlpTitlePlayers);
            // (an update, a system application, a step away: other titles)
            for (const u64 id : {u64{0x0004000E00000000ull | low}, u64{0x0004001000000000ull | low}, u64{app + 0x10000}, u64{app ^ 0x1}}) {
                if (title_rules::RowOf(id) >= 0 && title_rules::RowOf(id) != static_cast<int>(r)) {
                    continue; // (another row's id, by chance)
                }
                CHECK_EQ(title_rules::RowOf(id), -1);
                CHECK(!flow::IsDenseTitle(id));
                CHECK(!title_rules::SameGame(id, app));
                CHECK(title_rules::DlpOf(id).mode == DlpGameMode::None);
            }
            // every rule within its bound
            CHECK(want.same_frame_ms <= title_rules::kSameFrameMsMax);
            CHECK(want.hand_over_burst >= 1 && want.hand_over_burst <= title_rules::kHandOverBurstMax);
            CHECK(want.cap_drop_max <= title_rules::kCapDropMaxBound);
            CHECK(want.send_budget_bytes == 0 ||
                  (want.send_budget_bytes >= title_rules::kSendBudgetMin && want.send_budget_bytes <= title_rules::kSendBudgetMax));
        }
        CHECK(ids >= 1);
    }
    CHECK_EQ(title_rules::kHandOverBurstMax, static_cast<u32>(ENET_AZL_READ_MAX_DATAGRAMS)); // (a read pass is handed over in one round at the most)
    // Island Tour's row: the catalogue of 3DS Link (family ATS), JPN DD400, USA F8100, EUR F8200, TWN 119300, KOR 119B00, with its numbers.
    static const u64 kIslandTour[] = {0x00040000000DD400ull, 0x00040000000F8100ull, 0x00040000000F8200ull, 0x0004000000119300ull,
                                      0x0004000000119B00ull};
    for (const u64 id : kIslandTour) {
        CHECK(flow::IsDenseTitle(id));
        CHECK(flow::IsDenseTitle(id | 0x0000000100000000ull));
        CHECK(title_rules::SameGame(id, 0x00040000000F8200ull));
    }
    const title_rules::Rules island = title_rules::RulesFor(0x00040000000F8200ull);
    CHECK_EQ(island.same_frame_ms, 30u);
    CHECK_EQ(island.hand_over_burst, 16u);
    CHECK_EQ(island.cap_drop_max, 2048u);
    CHECK(island.host_keeps_to_its_game);
    CHECK_EQ(island.send_budget_bytes, 4096u);
    CHECK(!island.worker_off_core1 && !island.late_soc_block && !island.soc_guest);
    // titles with no row
    static const u64 kNoRow[] = {
        0x0004000000164800ull, // (the tests' game)
        0x0004001000022300ull, // (a system application's id)
        0,
        0xFFFFFFFFFFFFFFFFull,
    };
    for (const u64 id : kNoRow) {
        CHECK(!flow::IsDenseTitle(id));
        CHECK_EQ(title_rules::RowOf(id), -1);
        const title_rules::Rules none = title_rules::RulesFor(id);
        CHECK(none.same_frame_ms == 0 && none.hand_over_burst == 1 && none.cap_drop_max == 0 && !none.host_keeps_to_its_game && none.send_budget_bytes == 0 &&
              !none.worker_off_core1 && !none.late_soc_block && !none.soc_guest);
        CHECK_EQ(std::string(title_rules::NameOf(id)), std::string(""));
        CHECK(!title_rules::SameGame(id, id));
        CHECK(title_rules::DlpOf(id).mode == DlpGameMode::None);
    }
    // games that work as they are and have a row for their Download Play column only (1.1.4, L4: they were the plugin's table): every other
    // rule is off, as for a game with no row
    static const u64 kDownloadPlayOnly[] = {
        0x0004000000030700ull, // Mario Kart 7
        0x00040000001D1A00ull, // Luigi's Mansion
        0x000400000010C000ull, // Kirby Triple Deluxe
        0x000400000007C800ull, // Mario Tennis Open
    };
    std::size_t dlp_only = 0;
    for (const u64 id : kDownloadPlayOnly) {
        ++dlp_only;
        CHECK(title_rules::RowOf(id) >= 0);
        CHECK(!flow::IsDenseTitle(id));
        CHECK(title_rules::DlpOf(id).mode != DlpGameMode::None);
        for (const u64 each : {id, u64{id | 0x0000000100000000ull}}) {
            const title_rules::Rules none = title_rules::RulesFor(each);
            CHECK(none.same_frame_ms == 0 && none.hand_over_burst == 1 && none.cap_drop_max == 0 && !none.host_keeps_to_its_game &&
                  none.send_budget_bytes == 0 && !none.worker_off_core1 && !none.late_soc_block && !none.soc_guest);
            const title_rules::Rules start = title_rules::AppRulesFor(each);
            CHECK(!start.worker_off_core1 && !start.late_soc_block && !start.soc_guest);
        }
        CHECK(std::string(title_rules::NameOf(id)) != std::string(""));
    }
    CHECK_EQ(dlp_only, std::size_t(4));
    static_assert(flow::IsDenseTitle(0x00040000000F8200ull), "a constant expression");
    CHECK(flow::MoreThisRound(1, 16, true));
    CHECK(flow::MoreThisRound(15, 16, true));
    CHECK(!flow::MoreThisRound(16, 16, true));
    CHECK(!flow::MoreThisRound(1, 1, true));  // every other title: the one packet of the service call
    CHECK(!flow::MoreThisRound(1, 16, false)); // a session that is over hands nothing more over
}

// ---- the filter -----------------------------------------------------------------------------------------------------------------
void TestSameFrame() {
    std::printf("flow relief: the same-frame filter (equal bytes on the same channel within the window, nothing else)\n");
    std::vector<u8> a(584, 0x11), b(584, 0x11);
    b[583] ^= 1; // one bit of difference, in the last byte
    {
        flow::SameFrame f;
        CHECK(!f.Repeats(1, a.data(), 584, 1000, 30)); // the first goes
        CHECK(f.Repeats(1, a.data(), 584, 1005, 30));
        CHECK(f.Repeats(1, a.data(), 584, 1011, 30));
        CHECK(f.Repeats(1, a.data(), 584, 1029, 30));
        CHECK(!f.Repeats(1, a.data(), 584, 1030, 30)); // 30 ms after the one that WENT (not after the last one seen)
        CHECK(f.Repeats(1, a.data(), 584, 1059, 30));
        CHECK(!f.Repeats(1, a.data(), 584, 1060, 30));
        // a frame that differs goes at once, and is the one remembered
        CHECK(!f.Repeats(1, b.data(), 584, 1061, 30));
        CHECK(f.Repeats(1, b.data(), 584, 1062, 30));
        CHECK(!f.Repeats(1, a.data(), 584, 1063, 30)); // (two states that alternate are never held back)
        CHECK(!f.Repeats(1, b.data(), 584, 1064, 30));
        // the same bytes, one byte shorter
        CHECK(!f.Repeats(1, b.data(), 583, 1065, 30));
        CHECK(f.Repeats(1, b.data(), 583, 1066, 30));
        // an empty frame is a frame
        CHECK(!f.Repeats(1, b.data(), 0, 1067, 30));
        CHECK(f.Repeats(1, b.data(), 0, 1068, 30));
    }
    {
        // a difference in every single byte and in every bit position is seen (584 x 8 frames, each against the one before it in its window)
        flow::SameFrame f;
        std::vector<u8> x(584, 0);
        u32 now = 5000, missed = 0;
        CHECK(!f.Repeats(1, x.data(), 584, now, 1000000));
        for (std::size_t i = 0; i < x.size(); ++i) {
            for (int bit = 0; bit < 8; ++bit) {
                x[i] ^= static_cast<u8>(1 << bit);
                missed += f.Repeats(1, x.data(), 584, ++now, 1000000) ? 1 : 0;
                x[i] ^= static_cast<u8>(1 << bit);
                missed += f.Repeats(1, x.data(), 584, ++now, 1000000) ? 1 : 0; // (back to the first one: also a change)
            }
        }
        CHECK_EQ(missed, 0u);
        // two bytes swapped (what a plain sum would not see)
        std::vector<u8> y(64, 0);
        y[10] = 3;
        y[20] = 7;
        CHECK(!f.Repeats(2, y.data(), 64, now, 1000));
        std::swap(y[10], y[20]);
        CHECK(!f.Repeats(2, y.data(), 64, now + 1, 1000));
    }
    {
        // the channels do not mix: the game's two binds each have their own last frame
        flow::SameFrame f;
        CHECK(!f.Repeats(1, a.data(), 100, 100, 30));
        CHECK(!f.Repeats(243, a.data(), 100, 101, 30));
        CHECK(f.Repeats(1, a.data(), 100, 102, 30));
        CHECK(f.Repeats(243, a.data(), 100, 103, 30));
        // a third channel takes the place of the one that sent longer ago (channel 1 here), and that one starts again
        CHECK(!f.Repeats(7, a.data(), 100, 104, 30));
        CHECK(f.Repeats(7, a.data(), 100, 105, 30));
        CHECK(f.Repeats(243, a.data(), 100, 106, 30));
        CHECK(!f.Repeats(1, a.data(), 100, 107, 30));
    }
    {
        // the clock wraps (32 bits of ms: 49 days) and a window of 0 is "off": nothing is a repeat, nothing is remembered
        flow::SameFrame f;
        CHECK(!f.Repeats(1, a.data(), 100, 0xFFFFFFF0u, 30));
        CHECK(f.Repeats(1, a.data(), 100, 0x00000005u, 30)); // 21 ms later
        CHECK(!f.Repeats(1, a.data(), 100, 0x0000000Eu, 30)); // 30 ms later
        flow::SameFrame off;
        for (u32 t = 0; t < 10; ++t) {
            CHECK(!off.Repeats(1, a.data(), 100, t, 0));
        }
        CHECK(!off.Repeats(1, a.data(), 100, 10, 30)); // (nothing was remembered while it was off)
    }
}

// ---- the filter in the engine ---------------------------------------------------------------------------------------------------
int DataFramesOf(SentFrames& sent) {
    return sent.CountSecureData();
}

void TestSendTo() {
    std::printf("flow relief: UdsEngine::SendTo - off, every frame goes (every other title); on, a broadcast frame equal to the last one is not sent again\n");
    g_rules.Reset();
    MockRoom room;
    Console host(room, 1);
    SentFrames sent(host);
    u32 ev = 0;
    CHECK_EQ(host.engine->Initialize(MakeNode(0x1111, "Host", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    NetworkInfo ni = MakeNetworkInfo(4, 6);
    CHECK_EQ(host.engine->BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni)), 0u);
    std::vector<u8> a(124, 0x21), b(124, 0x21);
    b[60] = 0x22;
    const u8 kFlags = 0x03; // (what the game sends its dense frames with)
    TakeStats();
    // 1. Off (the default): six equal frames are six frames on the wire, with six numbers.
    sent.Clear();
    for (int i = 0; i < 6; ++i) {
        CHECK_EQ(host.engine->SendTo(kBroadcastNetworkNodeId, 1, 124, kFlags, a.data(), a.size()), 0u);
    }
    CHECK_EQ(DataFramesOf(sent), 6);
    Stats s = TakeStats();
    CHECK_EQ(s.tx_same_dropped, 0u);
    CHECK_EQ(s.sendto_calls, 6u);
    // 2. On (a window far longer than the test takes, so that no check is a race with the clock).
    host.engine->SetSameFrameWindow(60000);
    sent.Clear();
    for (int i = 0; i < 6; ++i) {
        CHECK_EQ(host.engine->SendTo(kBroadcastNetworkNodeId, 1, 124, kFlags, a.data(), a.size()), 0u); // the game is told "sent" every time
    }
    CHECK_EQ(DataFramesOf(sent), 1);
    s = TakeStats();
    CHECK_EQ(s.tx_same_dropped, 5u);
    CHECK_EQ(s.sendto_calls, 6u);
    CHECK_EQ(s.sendto_bytes, 6u * 124u); // (what the game asked for is still what the "sendto" line says)
    CHECK_EQ(s.sendto_errors, 0u);
    // a frame that differs goes at once; so does the first one when it comes back
    CHECK_EQ(host.engine->SendTo(kBroadcastNetworkNodeId, 1, 124, kFlags, b.data(), b.size()), 0u);
    CHECK_EQ(host.engine->SendTo(kBroadcastNetworkNodeId, 1, 124, kFlags, b.data(), b.size()), 0u);
    CHECK_EQ(host.engine->SendTo(kBroadcastNetworkNodeId, 1, 124, kFlags, a.data(), a.size()), 0u);
    CHECK_EQ(DataFramesOf(sent), 3);
    // another data channel has its own last frame
    CHECK_EQ(host.engine->SendTo(kBroadcastNetworkNodeId, 243, 124, kFlags, a.data(), a.size()), 0u);
    CHECK_EQ(host.engine->SendTo(kBroadcastNetworkNodeId, 243, 124, kFlags, a.data(), a.size()), 0u);
    CHECK_EQ(DataFramesOf(sent), 4);
    // the numbers of the frames that went are in a row: one that is held back uses none
    {
        std::vector<u16> numbers;
        for (const WifiPacket& p : sent.All()) {
            EtherType t;
            if (p.type == WifiPacket::Type::Data && GetFrameEtherType(p.data, &t) && t == EtherType::SecureData) {
                numbers.push_back(LoadBE16(p.data.data() + kLlcHeaderSize + 8)); // SecureDataHeader::sequence_number
            }
        }
        CHECK_EQ(numbers.size(), std::size_t(4));
        for (std::size_t i = 1; i < numbers.size(); ++i) {
            CHECK_EQ(numbers[i], static_cast<u16>(numbers[i - 1] + 1));
        }
    }
    // 3. A frame for ONE node is never held back (a unicast frame is the radio's to retry, and this node id is not in the network: no frame either way).
    sent.Clear();
    TakeStats();
    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(host.engine->SendTo(2, 1, 124, 0x01, a.data(), a.size()), 0u);
    }
    s = TakeStats();
    CHECK_EQ(s.tx_same_dropped, 0u);
    // 4. The window: with a short one the same frame goes again once it is over.
    host.engine->SetSameFrameWindow(40);
    sent.Clear();
    CHECK_EQ(host.engine->SendTo(kBroadcastNetworkNodeId, 1, 124, kFlags, b.data(), b.size()), 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK_EQ(host.engine->SendTo(kBroadcastNetworkNodeId, 1, 124, kFlags, b.data(), b.size()), 0u);
    CHECK_EQ(DataFramesOf(sent), 2);
    // 5. Off again: as at the start.
    host.engine->SetSameFrameWindow(0);
    sent.Clear();
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(host.engine->SendTo(kBroadcastNetworkNodeId, 1, 124, kFlags, b.data(), b.size()), 0u);
    }
    CHECK_EQ(DataFramesOf(sent), 4);
    host.engine->Shutdown();
    room.WaitIdle();
    CHECK_EQ(g_rules.violations().size(), std::size_t(0));
}

// ---- the hand-over burst ----------------------------------------------------------------------------------------------------------
/// The least a room has to be for a RoomClient: it takes the join and sends what the test gives it, reliable, a datagram each.
class TinyRoom {
public:
    TinyRoom() {
        ENetAddress address{};
        address.host = ENET_HOST_ANY;
        address.port = 0;
        host_ = enet_host_create(&address, 2, room::kNumChannels, 0, 0);
        ENetAddress bound{};
        enet_socket_get_address(host_->socket, &bound);
        port_ = bound.port;
        thread_ = std::thread([this] { Loop(); });
    }
    ~TinyRoom() {
        stop_ = true;
        thread_.join();
        enet_host_destroy(host_);
    }
    u16 port() const {
        return port_;
    }
    /// `count` beacons whose bodies are numbered from `first`, each flushed in a datagram of its own (as the room does).
    void SendNumbered(u32 first, u32 count) {
        std::lock_guard<std::mutex> l(m_);
        for (u32 i = 0; i < count; ++i) {
            WifiPacket p;
            p.type = WifiPacket::Type::Beacon; // (any type reaches the client's callback)
            p.channel = 11;
            p.transmitter_address = Mac{0, 0x1F, 0x32, 0, 0, 0x42};
            p.destination_address = kBroadcastMac;
            p.data.assign(200, 0x5A);
            const u32 n = first + i;
            std::memcpy(p.data.data(), &n, 4);
            outbox_.push_back(room::EncodeWifiPacket(p));
        }
    }

    /// The numbers of the client's frames the room got, in order.
    std::vector<u32> Received() {
        std::lock_guard<std::mutex> l(m_);
        return received_;
    }

private:
    void Loop() {
        while (!stop_.load()) {
            std::vector<std::vector<u8>> outgoing;
            {
                std::lock_guard<std::mutex> l(m_);
                outgoing.swap(outbox_);
            }
            for (const auto& message : outgoing) {
                if (peer_) {
                    enet_peer_send(peer_, 0, enet_packet_create(message.data(), message.size(), ENET_PACKET_FLAG_RELIABLE));
                    enet_host_flush(host_);
                }
            }
            ENetEvent event;
            while (enet_host_service(host_, &event, 2) > 0) {
                if (event.type == ENET_EVENT_TYPE_CONNECT) {
                    peer_ = event.peer;
                } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    if (event.packet->dataLength > 0 && event.packet->data[0] == room::IdJoinRequest) {
                        const u8 reply[] = {room::IdJoinSuccess, 0x00, 0x1F, 0x32, 0x00, 0x00, 0x99};
                        enet_peer_send(event.peer, 0, enet_packet_create(reply, sizeof(reply), ENET_PACKET_FLAG_RELIABLE));
                    } else {
                        WifiPacket p; // (the client's frames: their numbers, in the order the room got them)
                        if (room::DecodeWifiPacket(event.packet->data, event.packet->dataLength, &p) && p.data.size() >= 4) {
                            u32 n = 0;
                            std::memcpy(&n, p.data.data(), 4);
                            std::lock_guard<std::mutex> l(m_);
                            received_.push_back(n);
                        }
                    }
                    enet_packet_destroy(event.packet);
                }
            }
        }
    }
    ENetHost* host_ = nullptr;
    ENetPeer* peer_ = nullptr;
    u16 port_ = 0;
    std::atomic<bool> stop_{false};
    std::mutex m_;
    std::vector<std::vector<u8>> outbox_;
    std::vector<u32> received_;
    std::thread thread_;
};

struct BurstRun {
    std::vector<u32> order;      ///< the numbers of the frames as the client's callback got them
    std::vector<u32> per_round;  ///< how many each round of the network thread handed over (rounds that handed none are left out)
    Stats stats{};
};

/// A client joins, its network thread is held in its tick, the room sends `frames` numbered frames, the thread is let go: what each round hands over.
BurstRun RunBurst(u32 burst, u32 frames) {
    BurstRun run;
    TinyRoom server;
    RoomClient client;
    std::mutex m;
    std::atomic<bool> held{false}, in_hold{false};
    u32 in_round = 0;
    client.SetWifiCallback([&](const WifiPacket& p) {
        u32 n = 0;
        if (p.data.size() >= 4) {
            std::memcpy(&n, p.data.data(), 4);
        }
        std::lock_guard<std::mutex> l(m);
        run.order.push_back(n);
        ++in_round;
    });
    client.SetTickCallback([&](u64) { // (once a round, after the packets and the queue)
        {
            std::lock_guard<std::mutex> l(m);
            if (in_round != 0) {
                run.per_round.push_back(in_round);
                in_round = 0;
            }
        }
        if (held.load()) {
            in_hold = true;
            while (held.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            in_hold = false;
        }
    });
    if (burst != 0) {
        client.SetHandOverBurst(burst);
    }
    RoomClient::Config config;
    config.host = "127.0.0.1";
    config.port = server.port();
    config.nickname = "FlowTest";
    config.console_id_hash = "3ds-flowtest";
    config.game_name = "Test Game";
    config.game_id = 0x0004000000164800ULL;
    client.Start(config);
    CHECK(WaitFor([&] { return client.joined(); }));
    held = true;
    CHECK(WaitFor([&] { return in_hold.load(); }));
    TakeStats();
    server.SendNumbered(1, frames);
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); // (all of them are in the client's socket)
    held = false;
    CHECK(WaitFor([&] {
        std::lock_guard<std::mutex> l(m);
        return run.order.size() >= frames;
    }));
    WaitFor([&] {
        std::lock_guard<std::mutex> l(m);
        return in_round == 0;
    });
    run.stats = TakeStats();
    client.Stop();
    return run;
}

void TestHandOverBurst() {
    std::printf("flow relief: the hand-over burst - by default one packet of the room a round (every other title); with 16, the ones that are ready go together\n");
    constexpr u32 kFrames = 40;
    auto in_order = [](const std::vector<u32>& order, u32 n) {
        if (order.size() != n) {
            return false;
        }
        for (u32 i = 0; i < n; ++i) {
            if (order[i] != i + 1) {
                return false;
            }
        }
        return true;
    };
    auto most = [](const std::vector<u32>& v) {
        u32 top = 0;
        for (const u32 x : v) {
            top = x > top ? x : top;
        }
        return top;
    };
    // 1. The default: 40 packets, 40 rounds.
    {
        const BurstRun run = RunBurst(0, kFrames);
        CHECK(in_order(run.order, kFrames));
        CHECK_EQ(most(run.per_round), 1u);
        CHECK_EQ(run.per_round.size(), std::size_t(kFrames));
        CHECK_EQ(run.stats.rx_same_round, 0u);
        CHECK_EQ(run.stats.rx_round_max, 1u);
    }
    // 2. A share of 1 asked for by name is the same.
    {
        const BurstRun run = RunBurst(1, kFrames);
        CHECK(in_order(run.order, kFrames));
        CHECK_EQ(most(run.per_round), 1u);
        CHECK_EQ(run.stats.rx_same_round, 0u);
    }
    // 3. The relief's share: the first pass of ENet reads 32 datagrams, which two rounds hand over, 16 each; then the eight that are left.
    {
        const u32 kIslandBurst = title_rules::RulesFor(0x00040000000F8200ull).hand_over_burst;
        CHECK_EQ(kIslandBurst, 16u);
        const BurstRun run = RunBurst(kIslandBurst, kFrames);
        std::string rounds;
        for (const u32 n : run.per_round) {
            rounds += " " + std::to_string(n);
        }
        std::printf("  40 packets with a share of 16: rounds of%s\n", rounds.c_str());
        CHECK(in_order(run.order, kFrames)); // nothing lost, nothing out of order
        CHECK_EQ(most(run.per_round), kIslandBurst);
        CHECK(run.per_round.size() >= 3 && run.per_round.size() <= 6);
        CHECK(run.per_round.size() >= 2 && run.per_round[0] == 16 && run.per_round[1] == 16);
        CHECK_EQ(run.stats.rx_same_round, kFrames - static_cast<u32>(run.per_round.size()));
        CHECK_EQ(run.stats.rx_round_max, kIslandBurst);
        // (and the room's stream lost nothing to the limit of waiting data on the way)
        CHECK_EQ(run.stats.enet_cap_refused, 0u);
        CHECK_EQ(run.stats.enet_cap_admitted, 0u);
        CHECK(run.stats.enet_waiting_max >= 200u && run.stats.enet_waiting_max < 16u * 1024u);
    }
    // 4. A share larger than a pass: a round hands over what the pass read and no more is asked of the socket for it.
    {
        const BurstRun run = RunBurst(1000, kFrames);
        CHECK(in_order(run.order, kFrames));
        CHECK(most(run.per_round) <= 32u);
        CHECK(most(run.per_round) >= 8u);
    }
}

// ---- the send budget of a round (2026-11-15, N2 of the plan) ------------------------------------------------------------------------
struct SendRun {
    std::vector<u32> per_round; ///< the game's frames each round of the network thread handed to ENet (rounds that sent none left out)
    std::vector<u32> received;  ///< what the room got, in order
    Stats stats{};
};

/// A client joins, its network thread is held in its tick, the game queues `frames` numbered beacons of `size` bytes, the thread is let go.
SendRun RunSend(u32 budget, u32 frames, u32 size) {
    SendRun run;
    TinyRoom server;
    RoomClient client;
    std::mutex m;
    std::atomic<bool> held{false}, in_hold{false};
    bool recording = true;
    u32 sent_before = 0;
    client.SetTickCallback([&](u64) { // (the network thread, after the queue and the flush: the round's sends are in the counters)
        const u32 sent = g_stats.tx_reliable + g_stats.tx_unreliable;
        {
            std::lock_guard<std::mutex> l(m);
            if (recording && sent != sent_before) {
                run.per_round.push_back(sent - sent_before);
            }
            sent_before = sent;
        }
        if (held.load()) {
            in_hold = true;
            while (held.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            in_hold = false;
        }
    });
    client.SetSendBudget(budget);
    RoomClient::Config config;
    config.host = "127.0.0.1";
    config.port = server.port();
    config.nickname = "BudgetTest";
    config.console_id_hash = "3ds-budgettest";
    config.game_name = "Test Game";
    config.game_id = 0x0004000000164800ULL;
    client.Start(config);
    CHECK(WaitFor([&] { return client.joined(); }));
    held = true;
    CHECK(WaitFor([&] { return in_hold.load(); }));
    TakeStats(); // (the thread is held: nothing of it counts until it is let go)
    {
        std::lock_guard<std::mutex> l(m);
        run.per_round.clear();
        sent_before = g_stats.tx_reliable + g_stats.tx_unreliable;
    }
    for (u32 i = 1; i <= frames; ++i) {
        WifiPacket p;
        p.type = WifiPacket::Type::Beacon;
        p.channel = 11;
        p.transmitter_address = Mac{0, 0x1F, 0x32, 0, 0, 0x99};
        p.destination_address = kBroadcastMac;
        p.data.assign(size, 0x6B);
        std::memcpy(p.data.data(), &i, 4);
        client.SendWifiPacket(p);
    }
    held = false;
    CHECK(WaitFor([&] { return server.Received().size() >= frames; }));
    run.received = server.Received();
    {
        std::lock_guard<std::mutex> l(m);
        recording = false; // (the counters are taken now: the rounds after this one do not count)
    }
    run.stats = TakeStats();
    client.Stop();
    return run;
}

void TestSendBudget() {
    std::printf("send budget: by default a round hands ENet the whole queue (every other title); with a budget, at most that many bytes a round, in order, nothing lost\n");
    constexpr u32 kFrames = 20;
    auto in_order = [](const std::vector<u32>& v, u32 n) {
        if (v.size() != n) {
            return false;
        }
        for (u32 i = 0; i < n; ++i) {
            if (v[i] != i + 1) {
                return false;
            }
        }
        return true;
    };
    // 1. The default: 20 frames of 1000 bytes in one round.
    {
        const SendRun run = RunSend(0, kFrames, 1000);
        CHECK(in_order(run.received, kFrames));
        CHECK(!run.per_round.empty() && run.per_round[0] == kFrames);
        CHECK_EQ(run.stats.tx_budget_rounds, 0u);
    }
    // 2. Island Tour's budget: 4 KB a round, so four frames of 1000 bytes (and their envelope of the room) a round, then the next round.
    {
        const u32 budget = title_rules::RulesFor(0x00040000000F8200ull).send_budget_bytes;
        CHECK_EQ(budget, 4096u);
        const SendRun run = RunSend(budget, kFrames, 1000);
        std::string rounds;
        u32 most = 0, total = 0;
        for (const u32 n : run.per_round) {
            rounds += " " + std::to_string(n);
            most = n > most ? n : most;
            total += n;
        }
        std::printf("  20 frames of 1000 B with a budget of 4096 B: rounds of%s\n", rounds.c_str());
        CHECK(in_order(run.received, kFrames)); // nothing lost, nothing out of order
        CHECK_EQ(total, kFrames);
        CHECK_EQ(most, 4u);
        CHECK(run.per_round.size() >= 5);
        CHECK(run.stats.tx_budget_rounds >= 4u);
        CHECK_EQ(run.stats.tx_stale_drops, 0u);
    }
    // 3. A frame larger than the budget still goes, alone in its round.
    {
        const SendRun run = RunSend(title_rules::kSendBudgetMin, 4, 1500);
        CHECK(in_order(run.received, 4));
        for (const u32 n : run.per_round) {
            CHECK_EQ(n, 1u);
        }
    }
}

// ---- the ring first after an absence (2026-11-15, N3 of the plan) ------------------------------------------------------------------
void TestRingFirst() {
    std::printf("after an absence: a title with a send budget reads what waited in the ring before it sends its queue, %u ms at the most\n",
                static_cast<unsigned>(flow::kAwayDrainMaxMs));
    // the start of the backlog and the bound
    u64 since = 0;
    flow::NoteAwayBacklog(false, 1000, &since);
    CHECK_EQ(since, u64{0});
    CHECK(!flow::HoldsForTheRing(since, 1000));
    flow::NoteAwayBacklog(true, 1000, &since);
    CHECK_EQ(since, u64{1000});
    flow::NoteAwayBacklog(true, 1030, &since); // (the same backlog: its start stays)
    CHECK_EQ(since, u64{1000});
    CHECK(flow::HoldsForTheRing(since, 1000));
    CHECK(flow::HoldsForTheRing(since, 1000 + flow::kAwayDrainMaxMs - 1));
    CHECK(!flow::HoldsForTheRing(since, 1000 + flow::kAwayDrainMaxMs)); // (a room that never lets the socket empty holds the queue no longer)
    CHECK(!flow::HoldsForTheRing(since, 999));                           // (a clock that went back: no hold)
    flow::NoteAwayBacklog(false, 1100, &since);
    CHECK_EQ(since, u64{0});
    flow::NoteAwayBacklog(true, 0, &since); // (a clock at 0 still marks a start)
    CHECK(since != 0);
    // a client away 200 ms while the room sends 40 packets and the game queues 8 frames: with a budget, a round holds the queue first; without, none
    for (const u32 budget : {0u, title_rules::RulesFor(0x00040000000F8200ull).send_budget_bytes}) {
        TinyRoom server;
        RoomClient client;
        std::atomic<bool> held{false}, in_hold{false};
        client.SetTickCallback([&](u64) {
            if (held.load()) {
                in_hold = true;
                while (held.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                in_hold = false;
            }
        });
        std::atomic<u32> heard{0};
        client.SetWifiCallback([&](const WifiPacket&) { ++heard; });
        client.SetSendBudget(budget);
        RoomClient::Config config;
        config.host = "127.0.0.1";
        config.port = server.port();
        config.nickname = "RingTest";
        config.console_id_hash = "3ds-ringtest";
        config.game_name = "Test Game";
        config.game_id = 0x0004000000164800ULL;
        client.Start(config);
        CHECK(WaitFor([&] { return client.joined(); }));
        held = true;
        CHECK(WaitFor([&] { return in_hold.load(); }));
        TakeStats();
        server.SendNumbered(1, 40);
        for (u32 i = 1; i <= 8; ++i) {
            WifiPacket p;
            p.type = WifiPacket::Type::Beacon;
            p.channel = 11;
            p.transmitter_address = Mac{0, 0x1F, 0x32, 0, 0, 0x99};
            p.destination_address = kBroadcastMac;
            p.data.assign(600, 0x6C);
            std::memcpy(p.data.data(), &i, 4);
            client.SendWifiPacket(p);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // (the thread away: the room's packets wait in the socket)
        held = false;
        CHECK(WaitFor([&] { return server.Received().size() >= 8 && heard.load() >= 40; }));
        const std::vector<u32> got = server.Received();
        CHECK(got.size() == 8 && got.front() == 1 && got.back() == 8);
        const Stats s = TakeStats();
        std::printf("  budget %u: %lu rounds held the queue for the ring, %lu times away\n", static_cast<unsigned>(budget),
                    static_cast<unsigned long>(s.tx_drain_rounds), static_cast<unsigned long>(s.enet_away_episodes));
        CHECK(s.enet_away_episodes >= 1u);
        if (budget == 0) {
            CHECK_EQ(s.tx_drain_rounds, 0u);
        } else {
            CHECK(s.tx_drain_rounds >= 1u);
        }
        CHECK_EQ(s.tx_stale_drops, 0u);
        client.Stop();
    }
}

// ---- every row has the whole family of its game (2026-11-15, L2 of the plan) ------------------------------------------------------
struct CatalogTitle {
    u32 low;
    u32 family; ///< the three letters of the product code, 0xAABBCC
};
const CatalogTitle kCatalog[] = {
#include "title_families.inc"
};

void TestFamilies() {
    std::printf("the table of the titles: every row has every region of its game in the catalogue (Tests/title_families.inc), the left-out ones said\n");
    const std::size_t n = sizeof(kCatalog) / sizeof(kCatalog[0]);
    CHECK_EQ(n, std::size_t(5217));
    auto family_of = [&](u32 low) -> u32 {
        for (const CatalogTitle& t : kCatalog) {
            if (t.low == low) {
                return t.family;
            }
        }
        return 0;
    };
    auto left_out = [](u32 low) {
        for (const u32 x : title_rules::kLeftOut) {
            if (x == low && x != 0) {
                return true;
            }
        }
        return false;
    };
    for (std::size_t each = 0; each < title_rules::kRowCount; ++each) {
        const title_rules::Row& row = title_rules::kRows[each];
        const u32 family = family_of(row.low[0]);
        CHECK(family != 0); // (the row's first id is in the catalogue)
        std::size_t ids = 0;
        for (const u32 low : row.low) {
            if (low == 0) {
                continue;
            }
            ++ids;
            CHECK_EQ(family_of(low), family); // (one game a row)
            CHECK(!left_out(low));
        }
        std::size_t in_family = 0, missing = 0;
        for (const CatalogTitle& t : kCatalog) {
            if (t.family != family) {
                continue;
            }
            ++in_family;
            bool listed = false;
            for (const u32 low : row.low) {
                listed = listed || (low != 0 && low == t.low);
            }
            if (!listed && !left_out(t.low)) {
                ++missing;
                std::printf("  the row %s does not have 0x%08X of its family %c%c%c\n", row.name, static_cast<unsigned>(t.low), static_cast<char>(family >> 16),
                            static_cast<char>(family >> 8), static_cast<char>(family));
            }
        }
        CHECK_EQ(missing, std::size_t(0));
        CHECK(in_family >= ids);
    }
    // every id left out is of the family of a row (else it is a leftover that names nothing) and in no row
    for (const u32 low : title_rules::kLeftOut) {
        const u32 family = family_of(low);
        CHECK(family != 0);
        bool of_a_row = false;
        for (std::size_t each = 0; each < title_rules::kRowCount; ++each) {
            of_a_row = of_a_row || family_of(title_rules::kRows[each].low[0]) == family;
            for (const u32 listed : title_rules::kRows[each].low) {
                CHECK(listed == 0 || listed != low);
            }
        }
        CHECK(of_a_row);
    }
}

// ---- the statistics line ----------------------------------------------------------------------------------------------------------
void TestStatsLine() {
    std::printf("flow relief: the statistics line\n");
    Stats s{};
    char text[512];
    int n = flow::FormatStatsLine(s, false, text, sizeof(text));
    CHECK(n > 0 && n < static_cast<int>(sizeof(text)));
    CHECK_EQ(std::string(text),
             std::string("stats: room stream flow: lag max 0 ms (the room's send to the hand-over; 0 behind a hole), waiting data peak 0 KB of 192"
                         " | at the limit: 0 commands refused, 0 taken over it, 0 game frames dropped | dense-broadcast relief off: 0 same frames not sent, 0 packets"
                         " handed over behind another in a round (most in a round 0), 0 rounds sent at the send budget"));
    s.enet_lag_max_ms = 2412;
    s.enet_hol_max_ms = 1305;
    s.enet_waiting_max = 196707;
    s.enet_cap_refused = 973;
    s.enet_cap_admitted = 62;
    s.enet_cap_dropped = 1840;
    s.tx_same_dropped = 603;
    s.rx_same_round = 880;
    s.rx_round_max = 16;
    s.tx_budget_rounds = 0;
    n = flow::FormatStatsLine(s, true, text, sizeof(text));
    CHECK(n > 0 && n < static_cast<int>(sizeof(text)));
    CHECK_EQ(std::string(text),
             std::string("stats: room stream flow: lag max 2412 ms (the room's send to the hand-over; 1305 behind a hole), waiting data peak 193 KB"
                         " of 192 | at the limit: 973 commands refused, 62 taken over it, 1840 game frames dropped | dense-broadcast relief on: 603 same frames not sent, 880"
                         " packets handed over behind another in a round (most in a round 16), 0 rounds sent at the send budget"));
    s.tx_budget_rounds = 41;
    n = flow::FormatStatsLine(s, true, text, sizeof(text));
    CHECK(std::string(text).find("(most in a round 16), 41 rounds sent at the send budget") != std::string::npos);
    // every number at its largest still fits the plugin's buffer of the flow line (448: Plugin::LogStats)
    std::memset(&s, 0xFF, offsetof(Stats, t_net_service));
    n = flow::FormatStatsLine(s, true, text, sizeof(text));
    CHECK(n > 0 && n < 448);
    // the line of the losses: zeros, numbers, and every number at its largest still fits the plugin's buffer of the flow line (448)
    s = Stats{};
    n = flow::FormatLossLine(s, 0, text, sizeof(text)); // (a title whose drop at the limit is off)
    CHECK_EQ(std::string(text),
             std::string("stats: room stream losses: 0 sequence numbers skipped (0 of them in what waited while the network thread was away 100 ms or more)"
                         " | at the limit: 0 refused for the placeholders at their maximum, most held 0 of 0"
                         " | away: 0 times, most read after one 0 datagrams 0 KB, 0 of them sent again, the game's queue held 0 rounds for it"));
    s.enet_seq_skipped = 37;
    s.enet_seq_skipped_away = 21;
    s.enet_cap_full = 288;
    s.enet_cap_held_max = 2048;
    s.enet_away_episodes = 5;
    s.enet_away_dgrams_max = 41;
    s.enet_away_bytes_max = 30 * 1024 + 1; // (KB rounded up: 31)
    s.enet_away_dup_max = 17;
    s.tx_drain_rounds = 3;
    n = flow::FormatLossLine(s, title_rules::RulesFor(0x00040000000F8200ull).cap_drop_max, text, sizeof(text));
    CHECK_EQ(std::string(text),
             std::string("stats: room stream losses: 37 sequence numbers skipped (21 of them in what waited while the network thread was away 100 ms or more)"
                         " | at the limit: 288 refused for the placeholders at their maximum, most held 2048 of 2048"
                         " | away: 5 times, most read after one 41 datagrams 31 KB, 17 of them sent again, the game's queue held 3 rounds for it"));
    std::memset(&s, 0xFF, offsetof(Stats, t_net_service));
    n = flow::FormatLossLine(s, 0xFFFFFFFFu, text, sizeof(text));
    CHECK(n > 0 && n < 448);
}

} // namespace

int main() {
    CHECK_EQ(enet_initialize(), 0);
    TestTitles();
    TestFamilies();
    TestSameFrame();
    TestSendTo();
    TestHandOverBurst();
    TestSendBudget();
    TestRingFirst();
    TestStatsLine();
    enet_deinitialize();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
