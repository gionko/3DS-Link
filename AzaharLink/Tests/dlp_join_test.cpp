// The Download Play page of 3DS Link, its engine (1.1.4, route B: analysis/piano-1.1.4-2026-11-15/specifiche/8-INGRESSO-DA-3DSLINK.md 6.2, approved
// by the user on 2026-11-16): the app's own Core (ConfigApp/source/dlp_join.cpp: the adapter of the real Download Play, the lobby of any game, the
// book) on the in-memory room of Tests/mock_room.hpp, in front of the model of Azahar's Download Play host (Tests/azahar_dlp_host.hpp) and of OUR
// host (the adapter with H5's wait on, Island Tour's lobby), and its pure part (ConfigApp/source/dlp_join_logic.hpp). The app's network thread is
// the test's AppLink: the engine's frames go to a QUEUE that the thread sends at the start of each round, then the engine's Tick, then Core::Step
// (RoomClient's loop: service, the queue, the tick); its LeaveRoom drops what is still queued, as RoomClient::Stop's polite disconnect does (ENet
// throws the peer's queues away). Real time, about two minutes.
//   J1  Island Tour's Azahar host, its child Ready here (the table built by BuildTable): a Join row within 2.5 s (name, the block's game, 1/4); AskJoin
//       with the list in its state 2 -> Entering (the word says 1) -> 5, 12 s there (105 s with AZL_LONG=1); TakeReboot false in 5; START -> 6 -> 9;
//       TakeReboot: the model's MAC, its FIRST passphrase, [15] = 0, the prepared title 000400000F8F8200; the lobby 1 after it with no passphrase;
//       the engine given back whole; no content asked
//   J1b a new scan (SELECT) and A in one round of the network thread: the join adopts the host the list showed (the scan cleared the lobby's table)
//   J2  another version (the block's sizes are not the host's): Grey, AskJoin refused, no client in the host's network
//   J3  a host of Luigi's Mansion beside it: no row, Hidden; J4 a full host: never Join, gone from the list, refused
//   J1c Core::Look: the word, the join flag, the rounds and the refusals read under the Core's lock (the page's phase and its "not let in" come
//       from one look: Word() then Joining() could straddle the Step that ends a join - review of B4)
//   J4b the Core's HostOf: full from the beacon's counts (total == max; never with max 0), and a full copy the list kept refused after SELECT + A in
//       one round (full is the only guard there); a child whose record is ours but the index file has no row: Grey ("index not written"), refused
//   J5  B in the state 5: the host sees the node leave, the list starts again, the app stays in the room; the page left in the state 5 (after that
//       B) and during a probe: LeaveRounds before the room is left (D-B10) - the Deauthentication reaches the host although the room drops what is
//       still queued, and LeaveFlushed is never true while the Step of the leave queues it (the B's older release does not count)
//   J6  the host ends in the state 5: HostLeft within kHostGoneMs + 500, TakeReboot false
//   J7  OUR host of Island Tour with H5's wait on: the app is a member with kAppTid (AwaitsChildOf, copied from plugin.cpp by plugin_dlp_extract.py),
//       its HandOver (ns:s faked: 0 -> the prepared title; refused -> no jump), then four times of the exit: J7a out at 4.5 s and its child at +7 s,
//       J7b out at 5.5 s (taken for an Azahar: what the budget of D-B1.6 avoids), J7c never back (the cap of 15 s), J7d an Old 3DS's child at +13 s
//   J8  JoinGate on every Facts, AnyWork, ShowOf, PhaseOf, FriendSeedOf, HostsALobby (a member hosts a lobby by MAC, not by name)
//   J9  BuildTable and the files: the index file as the plugin reads it (cut at kIndexMaxBytes), the records, AM, the plugin's folder, the blocks
//   J10 the Core's block is zero after EndCore
//   J11 ComposeRebootInfo
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"  // (the rig's helpers of the other tests)
#include "dlp_net_rig.hpp"
#pragma GCC diagnostic pop

#include <unistd.h>
#include <sys/stat.h>

#include <algorithm>
#include <filesystem>

#include "azahar_dlp_host.hpp"
#include "azl/dlp_hub.hpp"
#include "azl/dlp_real_net.hpp"
#include "azl/title_rules.hpp"
#include "ctr/text_search_guard.hpp"
#include "dlp_guest_paths.hpp"
#include "dlp_join.hpp"

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

// AwaitsChildOf as the plugin has it (the slice of plugin.cpp Tests/plugin_dlp_extract.py cuts: the H5 filter of OnRoomInfo)
namespace azl {
namespace ctr {
namespace {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "plugin_dlp_slice.inc"
#pragma GCC diagnostic pop
}  // namespace
}  // namespace ctr
}  // namespace azl

namespace dlpjoin {
struct CoreTestAccess {
    static DlpLobby& lobby(Core& c) {
        return c.lobby_;
    }
    static RealDlpNet& net(Core& c) {
        return c.net_;
    }
    /// The Core's own HostOf (under its lock): what the list and DoJoin make of a group.
    static Host HostOf(Core& c, const Group& g) {
        platform::LockGuard<platform::Mutex> lock(c.mu_);
        return c.HostOf(g);
    }
    /// J4b: the list's copy of `host` says it is full now (as a beacon of a full host the list showed would): how many copies changed.
    static u32 FillSeen(Core& c, const Mac& host) {
        platform::LockGuard<platform::Mutex> lock(c.mu_);
        u32 n = 0;
        for (std::size_t i = 0; i < sizeof(c.seen_) / sizeof(c.seen_[0]); ++i) {
            if (c.seen_used_[i] && c.seen_[i].host == host) {
                c.seen_[i].total_nodes = c.seen_[i].max_nodes;
                ++n;
            }
        }
        return n;
    }
    /// J1c: `f` with the Core's lock held, as a Step holds it for its whole round.
    template <class F>
    static void UnderLock(Core& c, F f) {
        platform::LockGuard<platform::Mutex> lock(c.mu_);
        f();
    }
    /// J1c: what a Step that ends a join and refuses an AskJoin changes (under the lock: UnderLock).
    static void StepChanges(Core& c, bool joining) {
        c.joining_ = joining;
        c.rounds_.fetch_add(1);
        c.refusals_.fetch_add(1);
    }
};
}  // namespace dlpjoin

using dlpjoin::Core;
using dlpjoin::CoreTestAccess;
using dlpjoin::Host;
using dlpjoin::Phase;
using JShow = dlpjoin::Show;
namespace dc = azl::dlp_children;

static constexpr u64 kIslandChild = 0x00040001000F8200ULL;
static constexpr u64 kIslandGame = 0x00040000000F8200ULL;
static constexpr u64 kIslandPrepared = 0x000400000F8F8200ULL;  // (the user's formula, written out)
static constexpr u16 kIslandVersion = 1040;
static constexpr u64 kLuigiChild = 0x00040001001D1A00ULL;

static void Sleep(u32 ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
static void Pass(u8 k, u8 out[9]) {
    static const char* hex = "0123456789abcdef";
    for (u32 i = 0; i < 8; ++i) {
        out[i] = static_cast<u8>(hex[(k * 7 + i * 3) & 15]);
    }
    out[8] = 0;
}
static u32 LongWaitMs() {
    const char* l = std::getenv("AZL_LONG");
    return l != nullptr && l[0] == '1' ? 105000u : 12000u;
}

// ---- the children ---------------------------------------------------------------------------------------------------------------------

static dlpguest::Guest IslandGuest() {
    dlpguest::Guest g;
    g.game = kIslandGame;
    g.child = kIslandChild;
    g.prepared = kIslandPrepared;
    g.child_version = kIslandVersion;
    g.state = dlpguest::GuestState::Installed;
    return g;
}
static dc::Row IslandRow(u16 version = kIslandVersion) {
    dc::Row r{};
    r.child = kIslandChild;
    r.game = kIslandGame;
    r.prepared = kIslandPrepared;
    r.version = version;
    r.where = 'S';
    return r;
}
static std::string IndexText(std::initializer_list<dc::Row> rows) {
    std::string s = std::string(dc::kIndexHeader) + "\n";
    for (const dc::Row& r : rows) {
        char line[dc::kRowBytes + 1];
        CHECK(dc::FormatRow(r, line) != 0);
        s += line;
    }
    return s;
}
static void FillBlock(u8* out, u32 transfer, u32 required) {
    std::memset(out, 0, dc::kBlockBytes);
    const char* name = "Island Tour";
    for (u32 i = 0; name[i] != '\0'; ++i) {
        StoreLE16(out + dc::kBlockShortAt + 2 * i, static_cast<u8>(name[i]));
    }
    StoreLE32(out + dc::kBlockTransferAt, transfer);
    StoreLE32(out + dc::kBlockRequiredAt, required);
}
struct Kids {
    std::unique_ptr<dlpjoin::Children> ch{new dlpjoin::Children};
    std::vector<u64> missing;
};
/// Island Tour's child Ready here, through BuildTable: the index text, its record, AM and the plugin there, its block with these sizes (0: any).
static std::unique_ptr<Kids> IslandKids(u32 transfer = 0, u32 required = 0) {
    std::unique_ptr<Kids> k(new Kids);
    CHECK(dlpguest::Register(kIslandPrepared, dlpguest::kFormApplication));
    const std::string text = IndexText({IslandRow()});
    const std::vector<dlpguest::Guest> guests = {IslandGuest()};
    CHECK(dlpjoin::BuildTable(
        text.data(), text.size(), guests,
        [](u64, u16* v) {
            *v = kIslandVersion;
            return dlpguest::Presence::Present;
        },
        [](u64) { return true; },
        [transfer, required](u64, u8* out) {
            FillBlock(out, transfer, required);
            return true;
        },
        k->ch.get(), &k->missing));
    CHECK_EQ(dlpjoin::ReadyCount(k->ch->table), 1u);
    return k;
}

// ---- the app's console and its network thread --------------------------------------------------------------------------------------------

struct AppLink {
    AppLink(MockRoom& room, u8 last) : room_(room) {
        mac = {0x00, 0x1F, 0x32, 0x00, 0x00, last};
        UdsEngine::Transport tr;
        tr.send = [this](WifiPacket& p) {
            p.transmitter_address = mac;
            g_rules.Inspect(p);
            Core* const w = watch.load();
            if (w != nullptr && p.type == WifiPacket::Type::Deauthentication) {
                deauths.fetch_add(1);
                flushed_at_deauth.fetch_add(w->LeaveFlushed() ? 1u : 0u);  // (inside the Step that queues it: the leave must not be over yet)
            }
            std::lock_guard<std::mutex> l(q_);
            if (!left_) {
                queue_.push_back(p);
            }
        };
        tr.get_mac = [this] { return mac; };
        engine.reset(new UdsEngine(&kernel, tr));
        index_ = room_.AddMember(mac, [this](const WifiPacket& p) {
            if (drop && drop(p)) {
                return;
            }
            engine->OnWifiPacket(p);
        });
    }
    ~AppLink() {
        StopThread();
        if (!left_) {
            room_.RemoveMember(index_);
        }
    }
    void Run(Core* core) {
        core_ = core;
        thread_ = std::thread([this] {
            while (!stop_) {
                if (!held_) {
                    Flush();
                    const u64 now = platform::NowMs();
                    engine->Tick(now);
                    if (core_ != nullptr) {
                        core_->Step(now);
                    }
                }
                Sleep(3);
            }
        });
    }
    void Hold(bool on) {
        held_ = on;
    }
    /// RoomClient::Stop: the thread ends, what is still queued is dropped (the polite disconnect throws the peer's queues away), the room forgets us.
    void LeaveRoom() {
        StopThread();
        {
            std::lock_guard<std::mutex> l(q_);
            dropped += static_cast<u32>(queue_.size());
            queue_.clear();
            left_ = true;
        }
        room_.RemoveMember(index_);
    }
    UdsEngine& e() {
        return *engine;
    }
    Mac mac;
    FakeKernel kernel;
    std::unique_ptr<UdsEngine> engine;
    std::function<bool(const WifiPacket&)> drop;
    u32 dropped = 0;
    /// J5: the Deauthentications this app queues while `watch` is set, and how many of them found LeaveFlushed already true (D-B10: none may)
    std::atomic<Core*> watch{nullptr};
    std::atomic<u32> deauths{0}, flushed_at_deauth{0};

private:
    void StopThread() {
        stop_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    void Flush() {
        std::deque<WifiPacket> out;
        {
            std::lock_guard<std::mutex> l(q_);
            out.swap(queue_);
        }
        for (WifiPacket& p : out) {
            room_.Send(index_, p);
        }
    }
    MockRoom& room_;
    int index_ = -1;
    std::mutex q_;
    std::deque<WifiPacket> queue_;
    bool left_ = false;
    Core* core_ = nullptr;
    std::atomic<bool> stop_{false}, held_{false};
    std::thread thread_;
};

/// A Core on a block of its own (J10 reads the block after EndCore).
struct AppCore {
    AppCore(AppLink& link, const Kids& kids, const char* name = "Daisy") {
        block = ::operator new(sizeof(Core));
        std::u16string n;
        for (const char* p = name; *p != '\0'; ++p) {
            n.push_back(static_cast<char16_t>(*p));
        }
        core = dlpjoin::MakeCore(block, sizeof(Core), link.engine.get(), link.mac, n, &kids.ch->table, kids.missing);
        CHECK(core != nullptr);
    }
    ~AppCore() {
        End();
        ::operator delete(block);
    }
    /// EndCore: true when every byte of the block is zero after it (J10).
    bool End() {
        if (core == nullptr) {
            return true;
        }
        dlpjoin::EndCore(core);
        core = nullptr;
        const u8* b = static_cast<const u8*>(block);
        return std::all_of(b, b + sizeof(Core), [](u8 x) { return x == 0; });
    }
    Core* operator->() {
        return core;
    }
    void* block = nullptr;
    Core* core = nullptr;
};

static u32 State(Core& c) {
    return c.Word() >> 24;
}
/// The row of `host` in the list (nullptr = not listed), within `ms`.
static bool FindRow(Core& c, const Mac& host, u32 ms, Host* out, JShow want) {
    return WaitFor(ms, [&] {
        for (const Host& h : c.Hosts()) {
            if (h.g.host == host && dlpjoin::ShowOf(h) == want) {
                *out = h;
                return true;
            }
        }
        return false;
    });
}
static bool Clean(Core& c, AppLink& a) {
    RealDlpNet& n = CoreTestAccess::net(c);
    return !n.Holds() && !a.e().initialized() && !a.e().client_foreign_filter_enforced() && !n.Probing();
}
static std::string NameOf(const u16* n) {
    std::string s;
    for (u32 i = 0; i < kNameUnits && n[i] != 0; ++i) {
        s += static_cast<char>(n[i] < 0x80 ? n[i] : '?');
    }
    return s;
}
static void CheckHost(AzaharDlpHost& h) {
    for (const std::string& v : h.violations()) {
        std::printf("   model violation: %s\n", v.c_str());
    }
    CHECK(h.violations().empty());
}

/// The Azahar host's game: accepts the client 1.6 s after it is in (by hand), nothing else.
class Accepter {
public:
    explicit Accepter(AzaharDlpHost& h) : h_(h), t_([this] { Run(); }) {}
    ~Accepter() {
        stop_ = true;
        t_.join();
    }

private:
    void Run() {
        while (!stop_ && h_.client_state(2) == AzaharDlpHost::NotJoined) {
            Sleep(2);
        }
        for (u32 waited = 0; !stop_ && waited < 1600; waited += 2) {
            Sleep(2);
        }
        if (!stop_) {
            h_.AcceptClient(2);
        }
    }
    AzaharDlpHost& h_;
    std::atomic<bool> stop_{false};
    std::thread t_;
};

static void Ran(const char* part, int before, int least) {
    const int ran = g_checks - before;
    std::printf("   %s: %d checks\n", part, ran);
    CHECK(ran >= least);
}

// ---- J1, J10, J1b ------------------------------------------------------------------------------------------------------------------------

static void J1_IslandTour() {
    std::printf("J1: the app's page and an Azahar host of Island Tour, its child prepared here\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "Gionko", kIslandChild, 3, 11);
    std::unique_ptr<Kids> kids = IslandKids();
    AppLink a(room, 2);
    AppCore c(a, *kids);
    Ticker ticker({&h.e()});
    h.StartHosting(true, 6);
    a.Run(c.core);
    Host row{};
    const u64 t0 = platform::NowMs();
    CHECK(FindRow(*c.core, h.mac(), 2500, &row, JShow::Join));
    std::printf("   a Join row after %u ms: '%s', %u/%u\n", static_cast<u32>(platform::NowMs() - t0), NameOf(row.g.name).c_str(), row.g.total_nodes,
                row.g.max_nodes);
    CHECK_EQ(NameOf(row.g.name), std::string("Gionko"));
    CHECK_EQ(row.g.total_nodes, 1u);
    CHECK_EQ(row.g.max_nodes, 4u);
    CHECK(row.v == dc::Verdict::Ready);
    CHECK_EQ(row.prepared, kIslandPrepared);
    CHECK(row.block != nullptr && LoadLE16(row.block + dc::kBlockShortAt) == 'I');  // (the block's game: "Island Tour")
    CHECK_EQ(State(*c.core), static_cast<u32>(kCliScanning));
    Accepter acc(h);
    std::vector<Phase> seen;
    auto note = [&] {
        const Core::View v = c->Look();  // (as Session::phase: one look)
        const Phase p = dlpjoin::PhaseOf(v.word, v.joining, RoomClient::State::Joined);
        if (seen.empty() || seen.back() != p) {
            seen.push_back(p);
        }
    };
    c->AskJoin(h.mac());
    bool word1 = false;
    CHECK(WaitFor(4000, [&] {
        if (c->Joining()) {
            word1 = word1 || State(*c.core) == kCliIdle;
        }
        note();
        return State(*c.core) == kCliInSession;
    }));
    CHECK(word1);  // (the word says 1 while the join is under way: the Core's flag is what says Entering)
    CHECK(std::find(seen.begin(), seen.end(), Phase::Entering) != seen.end());
    u8 info[16];
    u64 prepared = 0;
    CHECK(!c->TakeReboot(info, &prepared));  // (not in the state 9: nothing)
    CHECK_EQ(prepared, 0u);
    const u32 wait = LongWaitMs();
    bool always5 = true;
    const u64 tw = platform::NowMs();
    while (platform::NowMs() - tw < wait) {
        always5 = always5 && State(*c.core) == kCliInSession;
        Sleep(20);
    }
    std::printf("   %u s in the lobby: always 5 = %d\n", wait / 1000, always5);
    CHECK(always5);
    CHECK(WaitFor(2000, [&] { return h.client_state(2) == AzaharDlpHost::Accepted; }));
    h.StartDistribution();
    u8 first[9];
    Pass(1, first);
    bool sent = false;
    CHECK(WaitFor(6000, [&] {
        note();
        if (!sent && h.state() == AzaharDlpHost::kNeedToSendPassphrase) {
            sent = h.BeginGame(first) == 0;
        }
        return State(*c.core) == kCliComplete;
    }));
    note();
    CHECK(std::find(seen.begin(), seen.end(), Phase::Waiting) != seen.end());
    CHECK(std::find(seen.begin(), seen.end(), Phase::Starting) != seen.end());
    CHECK(seen.back() == Phase::Passphrase);
    std::memset(info, 0xFF, sizeof(info));
    CHECK(c->TakeReboot(info, &prepared));
    CHECK(std::memcmp(info, h.mac().data(), 6) == 0);
    CHECK(std::memcmp(info + 6, first, 9) == 0);  // the FIRST passphrase of the host
    CHECK_EQ(info[15], 0u);
    CHECK_EQ(prepared, kIslandPrepared);
    CHECK(!c->TakeReboot(info, &prepared));  // (once)
    // the page leaves: the two rounds, then the room
    CHECK(dlpjoin::LeaveRounds(*c.core, &platform::NowMs, &platform::SleepMs));
    a.LeaveRoom();
    DlpLobby& lobby = CoreTestAccess::lobby(*c.core);
    CHECK_EQ(lobby.client_state(), static_cast<u8>(kCliIdle));
    u8 pass[9];
    lobby.ClientPassphrase(pass);
    CHECK(std::all_of(pass, pass + 9, [](u8 x) { return x == 0; }));
    CHECK(Clean(*c.core, a));
    CHECK_EQ(CoreTestAccess::net(*c.core).counters().content, 0u);
    CHECK_EQ(CoreTestAccess::net(*c.core).counters().host_bad, 0u);
    CHECK_EQ(c->refusals(), 0u);
    CheckHost(h);
    std::printf("J10: the Core's block after EndCore\n");
    CHECK(c.End());
    CHECK_RULES();
}

static void J1b_ScanThenJoin() {
    std::printf("J1b: SELECT and A in one round: the join adopts the host the list showed\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "Gionko", kIslandChild, 3, 13);
    std::unique_ptr<Kids> kids = IslandKids();
    AppLink a(room, 2);
    AppCore c(a, *kids);
    Ticker ticker({&h.e()});
    h.StartHosting(true, 6);
    a.Run(c.core);
    Host row{};
    CHECK(FindRow(*c.core, h.mac(), 2500, &row, JShow::Join));
    a.Hold(true);
    Sleep(20);
    c->AskScan();  // (the lobby's table is cleared: the host comes back only after a new probe)
    c->AskJoin(h.mac());
    a.Hold(false);
    CHECK(WaitFor(4000, [&] { return State(*c.core) == kCliInSession; }));
    CHECK_EQ(c->refusals(), 0u);
    CHECK_EQ(c->last_join_result(), 0u);
    CHECK(dlpjoin::LeaveRounds(*c.core, &platform::NowMs, &platform::SleepMs));
    a.LeaveRoom();
    CHECK(WaitFor(1500, [&] { return h.client_state(2) == AzaharDlpHost::NotJoined; }));
    CHECK(Clean(*c.core, a));
    CheckHost(h);
    CHECK_RULES();
}

// ---- J2, J3, J4 --------------------------------------------------------------------------------------------------------------------------

static void J2_OtherVersion() {
    std::printf("J2: the block's sizes are not the host's: another version, grey, refused\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "Gionko", kIslandChild, 3, 14);
    std::unique_ptr<Kids> kids = IslandKids(1, 1);
    AppLink a(room, 2);
    AppCore c(a, *kids);
    Ticker ticker({&h.e()});
    h.StartHosting(true, 6);
    a.Run(c.core);
    Host row{};
    CHECK(FindRow(*c.core, h.mac(), 2500, &row, JShow::Grey));
    CHECK(row.v == dc::Verdict::OtherVersion);
    c->AskJoin(h.mac());
    CHECK(WaitFor(1000, [&] { return c->refusals() == 1; }));
    Sleep(1500);
    CHECK_EQ(State(*c.core), static_cast<u32>(kCliScanning));
    CHECK(!c->Joining());
    CHECK_EQ(h.client_state(2), static_cast<u8>(AzaharDlpHost::NotJoined));  // (no client in the host's network)
    CHECK_EQ(h.sent("A1"), 0u);
    CHECK(dlpjoin::LeaveRounds(*c.core, &platform::NowMs, &platform::SleepMs));
    a.LeaveRoom();
    CHECK(Clean(*c.core, a));
    CheckHost(h);
    CHECK_RULES();
}

static void J3_J4_HiddenAndFull() {
    std::printf("J3/J4: a host of Luigi's Mansion (no row: hidden) and a full host of Island Tour (never Join, refused)\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "Gionko", kIslandChild, 1, 15);  // (one client: two nodes)
    AzaharDlpHost luigi(room, 3, "Luigi", kLuigiChild, 3, 16);
    std::unique_ptr<Kids> kids = IslandKids();
    AppLink a(room, 2);   // the first app joins: the host is full
    AppLink b(room, 4);
    AppCore ca(a, *kids, "Peach");
    AppCore cb(b, *kids, "Toad");
    Ticker ticker({&h.e(), &luigi.e()});
    h.StartHosting(true, 6);
    luigi.StartHosting(true, 6);
    a.Run(ca.core);
    b.Run(cb.core);
    Host row{};
    CHECK(FindRow(*ca.core, h.mac(), 2500, &row, JShow::Join));
    CHECK(FindRow(*cb.core, h.mac(), 2500, &row, JShow::Join));
    CHECK(FindRow(*cb.core, luigi.mac(), 3500, &row, JShow::Hidden));
    CHECK(row.v == dc::Verdict::NoRow);
    u32 hidden = 0;
    for (const Host& x : cb->Hosts()) {
        hidden += dlpjoin::ShowOf(x) == JShow::Hidden ? 1u : 0u;
    }
    CHECK_EQ(hidden, 1u);  // (counted at the bottom of the page)
    ca->AskJoin(h.mac());
    CHECK(WaitFor(4000, [&] { return State(*ca.core) == kCliInSession; }));
    // (the searcher drops a host whose beacon says total == max, R6f: the list shows it grey "full" for a moment at most, never Join)
    bool grey_full = false;
    CHECK(WaitFor(4000, [&] {
        bool listed = false;
        for (const Host& x : cb->Hosts()) {
            if (x.g.host == h.mac()) {
                listed = true;
                CHECK(x.full == (x.g.max_nodes != 0 && x.g.total_nodes >= x.g.max_nodes));  // (HostOf's full: the beacon's counts; J4b holds it)
                grey_full = grey_full || (x.full && dlpjoin::ShowOf(x) == JShow::Grey);
            }
        }
        return !listed;
    }));
    std::printf("   the full host left the list (seen grey 'full' first: %d)\n", grey_full);
    cb->AskJoin(h.mac());  // (refused because the list no longer has it; a full copy the list kept is J4b's)
    CHECK(WaitFor(1000, [&] { return cb->refusals() == 1; }));
    CHECK_EQ(State(*cb.core), static_cast<u32>(kCliScanning));
    cb->AskJoin(luigi.mac());
    CHECK(WaitFor(1000, [&] { return cb->refusals() == 2; }));
    CHECK_EQ(luigi.client_state(2), static_cast<u8>(AzaharDlpHost::NotJoined));
    for (AppLink* l : {&a, &b}) {
        CHECK(dlpjoin::LeaveRounds(l == &a ? *ca.core : *cb.core, &platform::NowMs, &platform::SleepMs));
        l->LeaveRoom();
    }
    CHECK(Clean(*ca.core, a));
    CHECK(Clean(*cb.core, b));
    CheckHost(h);
    CheckHost(luigi);
    CHECK_RULES();
}

static void J4b_HostOf() {
    std::printf("J4b: the Core's HostOf - full from the beacon's counts (a full copy of the list refused after a new scan), a child the index file lacks\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "Gionko", kIslandChild, 3, 21);
    std::unique_ptr<Kids> kids = IslandKids();
    // the second app: the record Installed, the index file written without its row ("index not written": BuildTable's missing_row)
    std::unique_ptr<Kids> unwritten(new Kids);
    {
        const std::string empty = IndexText({});
        const std::vector<dlpguest::Guest> guests = {IslandGuest()};
        CHECK(dlpjoin::BuildTable(
            empty.data(), empty.size(), guests,
            [](u64, u16* v) {
                *v = kIslandVersion;
                return dlpguest::Presence::Present;
            },
            [](u64) { return true; }, [](u64, u8* out) {
                FillBlock(out, 0, 0);
                return true;
            },
            unwritten->ch.get(), &unwritten->missing));
        CHECK_EQ(unwritten->missing.size(), 1u);
    }
    AppLink a(room, 2);
    AppLink b(room, 4);
    AppCore ca(a, *kids, "Peach");
    AppCore cb(b, *unwritten, "Toad");
    Ticker ticker({&h.e()});
    h.StartHosting(true, 6);
    a.Run(ca.core);
    b.Run(cb.core);
    Host row{};
    CHECK(FindRow(*ca.core, h.mac(), 2500, &row, JShow::Join));
    CHECK(!row.full && row.index_row);
    // HostOf on a beacon's counts: full at total == max, never with max 0
    Group g = row.g;
    g.total_nodes = g.max_nodes;
    const Host full = CoreTestAccess::HostOf(*ca.core, g);
    CHECK(full.full);
    CHECK(dlpjoin::ShowOf(full) == JShow::Grey);
    g.total_nodes = static_cast<u8>(g.max_nodes - 1);
    CHECK(!CoreTestAccess::HostOf(*ca.core, g).full);
    CHECK(dlpjoin::ShowOf(CoreTestAccess::HostOf(*ca.core, g)) == JShow::Join);
    g.max_nodes = 0;
    CHECK(!CoreTestAccess::HostOf(*ca.core, g).full);
    // the list's copy of the host full (its last beacon before the scan dropped it), then SELECT and A in one round: full is the only guard there
    a.Hold(true);
    Sleep(20);
    CHECK_EQ(CoreTestAccess::FillSeen(*ca.core, h.mac()), 1u);
    ca->AskScan();
    ca->AskJoin(h.mac());
    a.Hold(false);
    CHECK(WaitFor(1000, [&] { return ca->refusals() == 1; }));
    Sleep(1500);
    CHECK(!ca->Joining());
    CHECK_EQ(State(*ca.core), static_cast<u32>(kCliScanning));
    CHECK_EQ(h.client_state(2), static_cast<u8>(AzaharDlpHost::NotJoined));  // (a full copy of the list: no join sent)
    // "index not written": the row is grey (not hidden: the record is ours), the join refused
    CHECK(FindRow(*cb.core, h.mac(), 2500, &row, JShow::Grey));
    CHECK(!row.index_row);
    CHECK(row.v == dc::Verdict::NoRow);
    cb->AskJoin(h.mac());
    CHECK(WaitFor(1000, [&] { return cb->refusals() == 1; }));
    Sleep(500);
    CHECK_EQ(State(*cb.core), static_cast<u32>(kCliScanning));
    CHECK(h.client_state(2) == AzaharDlpHost::NotJoined && h.client_state(3) == AzaharDlpHost::NotJoined);  // (no client in the host's network)
    CHECK_EQ(h.sent("A1"), 0u);
    for (AppLink* l : {&a, &b}) {
        CHECK(dlpjoin::LeaveRounds(l == &a ? *ca.core : *cb.core, &platform::NowMs, &platform::SleepMs));
        l->LeaveRoom();
    }
    CHECK(Clean(*ca.core, a));
    CHECK(Clean(*cb.core, b));
    CheckHost(h);
    CHECK_RULES();
}

// ---- J5 ----------------------------------------------------------------------------------------------------------------------------------

static void J5_Leaves() {
    std::printf("J5: B in the state 5, the page left in the state 5 and during a probe (the room drops what is still queued)\n");
    g_rules.Reset();
    {
        MockRoom room;
        AzaharDlpHost h(room, 1, "Gionko", kIslandChild, 3, 17);
        std::unique_ptr<Kids> kids = IslandKids();
        AppLink a(room, 2);
        AppCore c(a, *kids);
        Ticker ticker({&h.e()});
        h.StartHosting(true, 6);
        a.Run(c.core);
        Host row{};
        CHECK(FindRow(*c.core, h.mac(), 2500, &row, JShow::Join));
        c->AskJoin(h.mac());
        CHECK(WaitFor(4000, [&] { return State(*c.core) == kCliInSession && h.client_state(2) != AzaharDlpHost::NotJoined; }));
        // B: the session goes, the list comes back, the app stays in the room
        c->AskLeave();
        CHECK(WaitFor(1500, [&] { return h.client_state(2) == AzaharDlpHost::NotJoined; }));
        CHECK(FindRow(*c.core, h.mac(), 4000, &row, JShow::Join));
        CHECK_EQ(State(*c.core), static_cast<u32>(kCliScanning));
        // in again, then the page goes in the state 5
        c->AskJoin(h.mac());
        CHECK(WaitFor(4000, [&] { return State(*c.core) == kCliInSession && h.client_state(2) != AzaharDlpHost::NotJoined; }));
        Sleep(3100);  // (a while in the lobby: the host's engine swallows ONE late goodbye within 3 s of a node's new association, kLateDeauthMs)
        // (the B above left a release behind: the leave counts from ITS release, not from that one - else the 3DS, slower inside ClientFinalize than
        // the PC, could see the old mark passed and leave the room while the Deauthentication is still being queued)
        a.watch.store(c.core);
        const bool rounds = dlpjoin::LeaveRounds(*c.core, &platform::NowMs, &platform::SleepMs);
        a.watch.store(nullptr);
        a.LeaveRoom();
        CHECK(rounds);
        std::printf("   the page left in 5 after a B: %u Deauthentications queued, %u of them with the leave already counted as over\n", a.deauths.load(),
                    a.flushed_at_deauth.load());
        CHECK(a.deauths.load() >= 1u);
        CHECK_EQ(a.flushed_at_deauth.load(), 0u);  // (D-B10: an earlier release never ends this leave)
        std::printf("   the page left in 5: %u frames still queued were dropped by the room\n", a.dropped);
        CHECK(WaitFor(1500, [&] { return h.client_state(2) == AzaharDlpHost::NotJoined; }));  // (the Deauthentication went out before the room)
        CHECK(Clean(*c.core, a));
        CheckHost(h);
    }
    {
        MockRoom room;
        AzaharDlpHost h(room, 1, "Gionko", kIslandChild, 3, 18);
        std::unique_ptr<Kids> kids = IslandKids();
        AppLink a(room, 2);
        AppCore c(a, *kids);
        Ticker ticker({&h.e()});
        h.StartHosting(true, 6);
        h.EndHosting();  // (no announcement: the probe stays connected, collecting, until kProbeCollectMs)
        a.Run(c.core);
        CHECK(WaitFor(3000, [&] { return h.e().SpectatorCount() == 1; }));
        CHECK(CoreTestAccess::net(*c.core).Probing());
        const bool rounds = dlpjoin::LeaveRounds(*c.core, &platform::NowMs, &platform::SleepMs);
        a.LeaveRoom();
        CHECK(rounds);
        CHECK(WaitFor(1500, [&] { return h.e().SpectatorCount() == 0; }));  // (the probe's Deauthentication reached the host)
        CHECK(Clean(*c.core, a));
        CheckHost(h);
    }
    CHECK_RULES();
}

// ---- J6 ----------------------------------------------------------------------------------------------------------------------------------

static void J6_HostGone() {
    std::printf("J6: the host ends in the state 5\n");
    g_rules.Reset();
    MockRoom room;
    AzaharDlpHost h(room, 1, "Gionko", kIslandChild, 3, 19);
    std::unique_ptr<Kids> kids = IslandKids();
    AppLink a(room, 2);
    AppCore c(a, *kids);
    Ticker ticker({&h.e()});
    h.StartHosting(true, 6);
    a.Run(c.core);
    Host row{};
    CHECK(FindRow(*c.core, h.mac(), 2500, &row, JShow::Join));
    c->AskJoin(h.mac());
    CHECK(WaitFor(4000, [&] { return State(*c.core) == kCliInSession; }));
    // (Azahar's host ends its network with no Deauthentication and leaves the room - the Session's room callback gives the engine the lists: the
    // engine drops the network once the room's lists have been without the host for 3 s, then the lobby's kHostGoneMs, as RC3)
    const std::vector<Mac> both = {h.mac(), a.mac}, alone = {a.mac};
    a.e().OnRoomMembers(both);
    a.e().OnRoomMembers(both);
    const u64 t0 = platform::NowMs();
    h.Finalize();
    CHECK(WaitFor(3000 + DlpLobby::kHostGoneMs + 1500, [&] {
        a.e().OnRoomMembers(alone);
        return dlpjoin::PhaseOf(c->Word(), c->Joining(), RoomClient::State::Joined) == Phase::HostLeft;
    }));
    const u32 gone = static_cast<u32>(platform::NowMs() - t0);
    std::printf("   HostLeft after %u ms\n", gone);
    CHECK(gone <= 3000 + DlpLobby::kHostGoneMs + 1500);
    u8 info[16];
    u64 prepared = 0;
    CHECK(!c->TakeReboot(info, &prepared));
    c->AskLeave();
    CHECK(WaitFor(1000, [&] { return State(*c.core) == kCliScanning; }));
    CHECK(dlpjoin::LeaveRounds(*c.core, &platform::NowMs, &platform::SleepMs));
    a.LeaveRoom();
    CHECK(Clean(*c.core, a));
    CHECK_RULES();
}

// ---- J7: our host with H5 ----------------------------------------------------------------------------------------------------------------

static u8 g_sent[16];
static s32 g_send_rc = 0;
static s32 FakeNsS(const u8 (&info)[16]) {
    std::memcpy(g_sent, info, sizeof(g_sent));
    return g_send_rc;
}

struct J7Plan {
    const char* what;
    u32 out_ms;   // the app leaves the room, ms after the host's 9
    u32 back_ms;  // its child is in the room (0 = never)
    u32 end_lo, end_hi;
    u8 back, stayed, missing;
    s32 send_rc;  // what ns:s answers
};

static void RunJ7(const J7Plan& p) {
    std::printf("J7: %s\n", p.what);
    g_rules.Reset();
    MockRoom room;
    Console hc(room, 1);
    RealDlpNet hn(hc.engine.get());
    LobbyConfig cfg = Player::Cfg(0x000F8200, 1, "Daisy", 4);
    DlpLobby hl(&hn, cfg);
    hn.SetIdentity(Me(0xF001, "Daisy"));
    hn.SetPlayers(4);
    hn.SetChildWait(true);  // (what MakeRealDlpHub gives a game that hosts the real Download Play)
    std::unique_ptr<Kids> kids = IslandKids();
    AppLink a(room, 2);
    AppCore c(a, *kids);
    Ticker ticker({hc.engine.get()});
    std::mutex hm;
    std::atomic<bool> hstop{false};
    std::thread hthread([&] {
        while (!hstop) {
            {
                std::lock_guard<std::mutex> l(hm);
                hn.Pump();
                hl.Pump();
            }
            Sleep(3);
        }
    });
    // the room as the plugin's OnRoomInfo gives it to the host's engine: everybody plays; AwaitsChildOf picks the guests the host waits for
    struct M {
        Mac mac;
        u64 game_id;
    };
    auto tell = [&](const std::vector<M>& members) {
        std::vector<Mac> all, dlp;
        for (const M& m : members) {
            all.push_back(m.mac);
            if (azl::ctr::AwaitsChildOf(m.game_id)) {
                dlp.push_back(m.mac);
            }
        }
        hc.engine->OnRoomMembers(all);
        hc.engine->SetDownloadPlayMembers(dlp);
    };
    const M host_m = {hc.mac, kIslandGame}, app_m = {a.mac, azl::title_rules::kAppTid}, child_m = {a.mac, kIslandPrepared};
    tell({host_m, app_m});
    {
        std::lock_guard<std::mutex> l(hm);
        CHECK_EQ(hl.ServerInitialize(3, 0, nullptr), 0u);
        CHECK_EQ(hl.ServerStartAccepting(true, 6), 0u);
    }
    a.Run(c.core);
    Host row{};
    CHECK(FindRow(*c.core, hc.mac, 3000, &row, JShow::Join));
    c->AskJoin(hc.mac);
    CHECK(WaitFor(4000, [&] {
        std::lock_guard<std::mutex> l(hm);
        u32 w = 0;
        return hl.ServerClientState(2, &w) == 0 && (w >> 24) == kPeerWaiting && hn.PeerState(2) == RealDlpNet::kAuthed;
    }));
    {
        std::lock_guard<std::mutex> l(hm);
        CHECK_EQ(hl.ServerAcceptClient(2), 0u);
        CHECK_EQ(hl.ServerStartDistribution(), 0u);
    }
    CHECK(WaitFor(3000, [&] {
        std::lock_guard<std::mutex> l(hm);
        return hl.server_state() == kSrvPassphrase;
    }));
    u8 nine[9];
    Pass(1, nine);
    {
        std::lock_guard<std::mutex> l(hm);
        CHECK_EQ(hl.ServerSendPassphrase(nine), 0u);
    }
    CHECK(WaitFor(4000, [&] {
        std::lock_guard<std::mutex> l(hm);
        return hl.server_state() == kSrvComplete;
    }));
    const u64 t9 = platform::NowMs();
    CHECK(WaitFor(3000, [&] { return State(*c.core) == kCliComplete; }));
    {
        std::lock_guard<std::mutex> l(hm);
        CHECK_EQ(hn.ChildrenAwaited(), 1u);
    }
    // the hand-over (ns:s faked)
    g_send_rc = p.send_rc;
    std::memset(g_sent, 0, sizeof(g_sent));
    s32 rc = 1;
    const u64 jump = dlpjoin::HandOver(*c.core, &FakeNsS, &rc);
    CHECK_EQ(rc, p.send_rc);
    CHECK_EQ(jump, p.send_rc == 0 ? kIslandPrepared : 0u);  // (a refusal of ns:s: no jump)
    CHECK(std::memcmp(g_sent, hc.mac.data(), 6) == 0 && std::memcmp(g_sent + 6, nine, 9) == 0 && g_sent[15] == 0);
    CHECK_EQ(State(*c.core), static_cast<u32>(kCliIdle));
    bool out = false, back = false;
    u32 ended = 0;
    for (;;) {
        const u32 at = static_cast<u32>(platform::NowMs() - t9);
        if (!out && at >= p.out_ms) {
            CHECK(dlpjoin::LeaveRounds(*c.core, &platform::NowMs, &platform::SleepMs));
            a.LeaveRoom();
            out = true;
        }
        if (out && !back && p.back_ms != 0 && at >= p.back_ms) {
            back = true;
        }
        tell(back ? std::vector<M>{host_m, child_m} : out ? std::vector<M>{host_m} : std::vector<M>{host_m, app_m});
        u32 total = 0;
        {
            std::lock_guard<std::mutex> l(hm);
            u8 ids[8];
            CHECK_EQ(hl.ServerConnectingClients(ids, 8, &total), 0u);
        }
        if (total == 0) {
            ended = at;
            break;
        }
        if (at > dlp_real::ChildWait::kMaxMs + 3000) {
            break;
        }
        Sleep(16);
    }
    dlp_real::ChildWait::End end{};
    CHECK(WaitFor(500, [&] { return hn.TakeChildWaitEnd(&end); }));
    std::printf("   the host's game read 0 after %u ms (wanted %u..%u): back %u, stayed %u, missing %u\n", ended, p.end_lo, p.end_hi, end.back, end.stayed,
                end.missing);
    CHECK(ended >= p.end_lo && ended <= p.end_hi);
    CHECK_EQ(end.back, p.back);
    CHECK_EQ(end.stayed, p.stayed);
    CHECK_EQ(end.missing, p.missing);
    if (!out) {
        CHECK(dlpjoin::LeaveRounds(*c.core, &platform::NowMs, &platform::SleepMs));
        a.LeaveRoom();
    }
    CHECK(Clean(*c.core, a));
    CHECK_EQ(hn.counters().content, 0u);
    hstop = true;
    hthread.join();
    {
        std::lock_guard<std::mutex> l(hm);
        hl.ServerFinalize();
    }
    CHECK_RULES();
}

static void J7_OurHost() {
    using CW = dlp_real::ChildWait;
    CHECK(azl::ctr::AwaitsChildOf(azl::title_rules::kAppTid));
    CHECK(!azl::ctr::AwaitsChildOf(kIslandPrepared));
    const J7Plan a = {"J7a, the app out at 4.5 s, its child at +7 s", 4500, 7000, 6900, 7000 + 1000, 1, 0, 0, 0};
    const J7Plan b = {"J7b, the app out at 5.5 s: taken for an Azahar (ns:s refused: no jump)", 5500, 0, CW::kLeaveMs - 100, CW::kLeaveMs + 600, 0, 1, 0,
                      static_cast<s32>(0xC8A0CFF0)};
    const J7Plan c = {"J7c, the app out at 4.5 s, its child never back: the cap", 4500, 0, CW::kMaxMs - 100, CW::kMaxMs + 600, 0, 0, 1, 0};
    const J7Plan d = {"J7d, an Old 3DS: the child at +13 s (its first frame)", 4500, 13000, 12900, 13000 + 1000, 1, 0, 0, 0};
    for (const J7Plan* p : {&a, &b, &c, &d}) {
        RunJ7(*p);
    }
}

// ---- J8, J11: the pure part --------------------------------------------------------------------------------------------------------------

static void J1c_Look() {
    std::printf("J1c: Core::Look reads its four values under the Core's lock\n");
    g_rules.Reset();
    MockRoom room;
    std::unique_ptr<Kids> kids = IslandKids();
    AppLink a(room, 2);  // (no network thread: the test plays the Step)
    AppCore c(a, *kids);
    const Core::View before = c->Look();
    CHECK(!before.joining);
    std::atomic<bool> done{false};
    Core::View seen;
    std::thread reader;
    CoreTestAccess::UnderLock(*c.core, [&] {
        reader = std::thread([&] {
            seen = c->Look();
            done = true;
        });
        Sleep(150);
        CHECK(!done.load());  // (Look waits for the Step under way: half of it is never read)
        CoreTestAccess::StepChanges(*c.core, true);
    });
    reader.join();
    CHECK(done.load());
    CHECK(seen.joining);  // (the Step's flag, with the Step's rounds and refusals: one instant)
    CHECK_EQ(seen.rounds, before.rounds + 1);
    CHECK_EQ(seen.refusals, before.refusals + 1);
    CHECK_EQ(seen.word, before.word);
    CHECK_RULES();
}

static void J8_Logic() {
    std::printf("J8: JoinGate, AnyWork, ShowOf, PhaseOf, FriendSeedOf, HostsALobby\n");
    using dlpjoin::Why;
    u32 cases = 0, ok = 0;
    for (u32 bits = 0; bits < (1u << 12); ++bits) {
        dlpjoin::Facts f;
        bool* b[11] = {&f.installed_app, &f.luma_ok, &f.loader_on, &f.plugin_on, &f.room_set, &f.autoconnect, &f.menu_off, &f.home_menu, &f.work_running,
                       &f.guest_worker, &f.delivery_up};
        for (u32 i = 0; i < 11; ++i) {
            *b[i] = ((bits >> i) & 1u) != 0;
        }
        f.guests_ready = (bits >> 11) & 1u;
        // the oracle, written out: the order of the enum, the plugin's UsesRoom spelled again
        const Why want = !f.installed_app ? Why::NotInstalled
                         : !f.luma_ok     ? Why::LumaTooOld
                         : !f.loader_on   ? Why::LoaderOff
                         : !f.plugin_on   ? Why::PluginOff
                         : !f.room_set    ? Why::NoRoom
                         : !(f.room_set && (f.autoconnect || f.menu_off)) ? Why::NoAutoConnect
                         : !f.home_menu   ? Why::NoHomeMenu
                         : f.work_running ? Why::WorkRunning
                         : f.guest_worker ? Why::GuestWorker
                         : f.delivery_up  ? Why::DeliveryUp
                         : f.guests_ready == 0 ? Why::NoGuests
                                               : Why::Ok;
        ++cases;
        ok += dlpjoin::JoinGate(f) == want ? 1u : 0u;
        CHECK(dlpjoin::JoinGate(f) == want);
    }
    std::printf("   JoinGate: %u of %u cases\n", ok, cases);
    CHECK_EQ(cases, 4096u);
    for (u32 w = 0; w <= static_cast<u32>(Why::NoGuests); ++w) {
        const std::string t = dlpjoin::WhyText(static_cast<Why>(w));
        CHECK(w == 0 ? t.empty() : (!t.empty() && t.size() <= 80));
    }
    CHECK(std::string(dlpjoin::WhyText(Why::PluginOff)) == "Turn 3DS Link on in Settings");
    // (1.1.4, spec 9 point 10) the refusal says where a guest is prepared: Download Play > Enable games (its row was "Games" until 2026-11-18),
    // never Enable Titles (its button opens Enable games)
    CHECK(std::string(dlpjoin::WhyText(Why::NoGuests)).find("Download Play > Enable games") != std::string::npos);
    CHECK(std::string(dlpjoin::WhyText(Why::NoGuests)).find("Enable Titles") == std::string::npos);
    for (u32 bits = 0; bits < 16; ++bits) {
        const dlpjoin::Work w = {(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0};
        CHECK_EQ(dlpjoin::AnyWork(w), bits != 0);
        // (the review of the build 2h, U3) the entrance waits only for the update and the public lobbies: never for the copies or the scan
        CHECK_EQ(dlpjoin::EntryWork(w), (bits & 12) != 0);
    }
    for (u32 v = 0; v <= static_cast<u32>(dc::Verdict::OtherVersion); ++v) {
        for (u32 full = 0; full < 2; ++full) {
            for (u32 idx = 0; idx < 2; ++idx) {
                Host h{};
                h.v = static_cast<dc::Verdict>(v);
                h.full = full != 0;
                h.index_row = idx != 0;
                const JShow want = idx == 0 ? JShow::Grey
                                  : h.v == dc::Verdict::NoRow ? JShow::Hidden
                                  : (h.v == dc::Verdict::Ready && full == 0) ? JShow::Join
                                                                             : JShow::Grey;
                CHECK(dlpjoin::ShowOf(h) == want);
            }
        }
    }
    const RoomClient::State rooms[] = {RoomClient::State::Idle, RoomClient::State::Connecting, RoomClient::State::Joining, RoomClient::State::Joined};
    const u8 states[] = {kCliOff, kCliIdle, kCliScanning, kCliInSession, kCliStarted, kCliComplete, kCliLost};
    for (const RoomClient::State r : rooms) {
        for (const u8 s : states) {
            for (u32 j = 0; j < 2; ++j) {
                const u32 word = static_cast<u32>(s) << 24;
                Phase want = Phase::Scanning;
                if (r == RoomClient::State::Idle) {
                    want = Phase::RoomLost;
                } else if (r != RoomClient::State::Joined) {
                    want = Phase::Joining;
                } else if (j != 0) {
                    want = Phase::Entering;
                } else {
                    want = s == kCliOff ? Phase::Off
                           : s == kCliInSession ? Phase::Waiting
                           : s == kCliStarted   ? Phase::Starting
                           : s == kCliComplete  ? Phase::Passphrase
                           : s == kCliLost      ? Phase::HostLeft
                                                : Phase::Scanning;
                }
                CHECK(dlpjoin::PhaseOf(word, j != 0, r) == want);
            }
        }
    }
    const Mac foobar = {'f', 'o', 'o', 'b', 'a', 'r'};  // (plugin_dlp_test's vector: FNV-1a 64 of "foobar")
    CHECK_EQ(dlpjoin::FriendSeedOf(foobar), 0x85944171F73967E8ull);
    // HostsALobby: an Azahar in the room as "MarioFan92" announces its lobby as "AZAHAR" (the console's profile name) with its room MAC
    {
        const Mac azahar = {0x02, 0x00, 0x00, 0x00, 0x00, 0x07};
        const Mac other = {0x02, 0x00, 0x00, 0x00, 0x00, 0x08};
        std::vector<Host> hosts(2);
        hosts[0] = Host{};
        hosts[1] = Host{};
        hosts[0].g.host = {0x02, 0x00, 0x00, 0x00, 0x00, 0x05};
        hosts[1].g.host = azahar;
        const char* beacon = "AZAHAR";
        for (u32 i = 0; beacon[i] != '\0'; ++i) {
            hosts[1].g.name[i] = static_cast<u16>(beacon[i]);
        }
        CHECK(NameOf(hosts[1].g.name) != std::string("MarioFan92"));  // (by name it would be a second, grey row of the same player)
        CHECK(dlpjoin::HostsALobby(hosts, azahar));
        CHECK(!dlpjoin::HostsALobby(hosts, other));
        CHECK(!dlpjoin::HostsALobby(std::vector<Host>(), azahar));
    }
}

static void J11_Compose() {
    std::printf("J11: ComposeRebootInfo\n");
    const u8 mac[6] = {0x00, 0x1F, 0x32, 0xAA, 0xBB, 0xCC};
    u8 pass[9];
    Pass(3, pass);
    pass[8] = 'v';
    u8 out[16];
    std::memset(out, 0xFF, sizeof(out));
    dlpjoin::ComposeRebootInfo(mac, pass, out);
    CHECK(std::memcmp(out, mac, 6) == 0);
    CHECK(std::memcmp(out + 6, pass, 9) == 0);
    CHECK_EQ(out[15], 0u);
}

// J12 (the final review of 1.1.4): the end of the session's parts - freed only when RoomClient::Stop saw the network thread end; a thread it
// abandoned keeps them (it may run one more round of the callbacks that use them)
static void J12_EndNetParts() {
    std::printf("J12: EndNetParts: the parts freed only when the network thread ended\n");
    for (const bool ended : {true, false}) {
        int stops = 0;
        int frees = 0;
        const bool freed = dlpjoin::EndNetParts(
            [&] {
                ++stops;
                return ended;
            },
            [&] { ++frees; });
        CHECK_EQ(stops, 1);
        CHECK_EQ(frees, ended ? 1 : 0);  // (abandoned: never freed under the thread)
        CHECK(freed == ended);
    }
}

// ---- J9: the table and the files ---------------------------------------------------------------------------------------------------------

struct Fakes {
    dlpguest::Presence at = dlpguest::Presence::Present;
    u16 version = kIslandVersion;
    bool plugin = true;
    bool block = true;
};
static bool Build(const std::string* text, const std::vector<dlpguest::Guest>& guests, const Fakes& fk, Kids* k) {
    return dlpjoin::BuildTable(
        text != nullptr ? text->data() : nullptr, text != nullptr ? text->size() : 0, guests,
        [&fk](u64, u16* v) {
            *v = fk.version;
            return fk.at;
        },
        [&fk](u64) { return fk.plugin; },
        [&fk](u64, u8* out) {
            FillBlock(out, 0, 0);
            return fk.block;
        },
        k->ch.get(), &k->missing);
}
static dc::Verdict RowVerdict(const Kids& k, u64 child) {
    const int r = dc::FindRow(k.ch->table.index, child);
    return r < 0 ? dc::Verdict::NoRow : static_cast<dc::Verdict>(k.ch->table.verdict[r]);
}

static void J9_Table() {
    std::printf("J9: BuildTable and the files of the plugin\n");
    CHECK(dlpguest::Register(kIslandPrepared, dlpguest::kFormApplication));
    const std::string text = IndexText({IslandRow()});
    const std::vector<dlpguest::Guest> one = {IslandGuest()};
    {
        Kids k;
        CHECK(Build(&text, one, Fakes(), &k));
        CHECK(RowVerdict(k, kIslandChild) == dc::Verdict::Ready);
        CHECK(dc::BlockStateOf(&k.ch->table, 0) == dc::BlockState::Read);  // (every block read at once: never "not read" in the app)
        CHECK_EQ(dlpjoin::ReadyCount(k.ch->table), 1u);
        CHECK(k.missing.empty());
    }
    {  // the record Installed, the file without its row: not Ready, "index not written"
        Kids k;
        const std::string empty = IndexText({});
        CHECK(Build(&empty, one, Fakes(), &k));
        CHECK(RowVerdict(k, kIslandChild) == dc::Verdict::NoRow);
        CHECK_EQ(dlpjoin::ReadyCount(k.ch->table), 0u);
        CHECK_EQ(k.missing.size(), 1u);
        CHECK(!k.missing.empty() && k.missing[0] == kIslandChild);
        Kids none;
        CHECK(!Build(nullptr, one, Fakes(), &none));  // (no file at all)
        CHECK_EQ(none.missing.size(), 1u);
        CHECK_EQ(dlpjoin::ReadyCount(none.ch->table), 0u);
    }
    {  // the file's version is not the record's
        Kids k;
        const std::string other = IndexText({IslandRow(kIslandVersion + 16)});
        Fakes fk;
        fk.version = kIslandVersion + 16;
        CHECK(Build(&other, one, fk, &k));
        CHECK(RowVerdict(k, kIslandChild) != dc::Verdict::Ready);
        CHECK_EQ(dlpjoin::ReadyCount(k.ch->table), 0u);
    }
    {  // a record Preparing: no row expected, nothing missing, nothing ready
        Kids k;
        dlpguest::Guest g = IslandGuest();
        g.state = dlpguest::GuestState::Preparing;
        const std::string empty = IndexText({});
        CHECK(Build(&empty, {g}, Fakes(), &k));
        CHECK(k.missing.empty());
        Kids k2;
        CHECK(Build(&text, {g}, Fakes(), &k2));  // (a row the record does not back: never Ready)
        CHECK(RowVerdict(k2, kIslandChild) == dc::Verdict::NotPrepared);
    }
    {  // AM: Unknown, Broken, Absent, another version -> never Ready
        for (const dlpguest::Presence at : {dlpguest::Presence::Unknown, dlpguest::Presence::Broken, dlpguest::Presence::Absent}) {
            Kids k;
            Fakes fk;
            fk.at = at;
            CHECK(Build(&text, one, fk, &k));
            CHECK(RowVerdict(k, kIslandChild) == dc::Verdict::NotPrepared);
        }
        Kids k;
        Fakes fk;
        fk.version = kIslandVersion - 1;
        CHECK(Build(&text, one, fk, &k));
        CHECK(RowVerdict(k, kIslandChild) == dc::Verdict::NotPrepared);
    }
    {  // the plugin's folder empty; no block
        Kids k;
        Fakes fk;
        fk.plugin = false;
        CHECK(Build(&text, one, fk, &k));
        CHECK(RowVerdict(k, kIslandChild) == dc::Verdict::NoChildPlugin);
        Kids k2;
        Fakes fk2;
        fk2.block = false;
        CHECK(Build(&text, one, fk2, &k2));
        CHECK(dc::BlockStateOf(&k2.ch->table, 0) == dc::BlockState::None);
        CHECK(dc::VerdictFor(&k2.ch->table, kIslandChild, 0, 0) == dc::Verdict::NotPrepared);
        CHECK_EQ(dlpjoin::ReadyCount(k2.ch->table), 0u);
    }
    {  // a title that is not ours (no slot of the registry): never Ready
        Kids k;
        dlpguest::Unregister(kIslandPrepared);
        CHECK(Build(&text, one, Fakes(), &k));
        CHECK(RowVerdict(k, kIslandChild) == dc::Verdict::NotPrepared);
        CHECK(k.missing.empty());
        CHECK(dlpguest::Register(kIslandPrepared, dlpguest::kFormApplication));
    }
    // the files: the index the plugin reads (sdmc: + kIndexPath), the records, the blocks; a temporary folder called "sdmc:"
    char old[1024];
    CHECK(getcwd(old, sizeof(old)) != nullptr);
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("dlp_join_test_" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "sdmc:" / "3ds" / "3DSLink" / "dlp-guests");
    CHECK(chdir(dir.string().c_str()) == 0);
    dlpguest::SetThisId0(std::string("0123456789abcdef") + "0123456789abcdef"); // (DPAPP-2j, R-B: the folder of this console's records)
    CHECK(dlpguest::SaveGuest(IslandGuest()));
    {
        u8 block[dc::kBlockBytes];
        FillBlock(block, 0, 0);
        FILE* f = std::fopen(dlpguest::BlockPath(kIslandPrepared).c_str(), "wb");
        CHECK(f != nullptr);
        if (f != nullptr) {
            std::fwrite(block, 1, sizeof(block), f);
            std::fclose(f);
        }
    }
    auto write_index = [](const std::string& t) {
        FILE* f = std::fopen((std::string("sdmc:") + dc::kIndexPath).c_str(), "wb");
        CHECK(f != nullptr);
        if (f != nullptr) {
            std::fwrite(t.data(), 1, t.size(), f);
            std::fclose(f);
        }
    };
    const dlpjoin::PresenceFn present = [](u64, u16* v) {
        *v = kIslandVersion;
        return dlpguest::Presence::Present;
    };
    const dlpjoin::FileFn plugin = [](u64) { return true; };
    {
        write_index(text);
        Kids k;
        CHECK(dlpjoin::LoadChildren(present, plugin, k.ch.get(), &k.missing));
        CHECK(RowVerdict(k, kIslandChild) == dc::Verdict::Ready);
        CHECK(dc::BlockStateOf(&k.ch->table, 0) == dc::BlockState::Read);
        CHECK(k.ch->table.index_found);
    }
    {  // a file past kIndexMaxBytes: read as the plugin reads it - the row after the cut is not there
        std::string big = std::string(dc::kIndexHeader) + "\n";
        while (big.size() < dc::kIndexMaxBytes - 20) {
            big += "#a later version's line\n";
        }
        const std::string row = IndexText({IslandRow()}).substr(sizeof(dc::kIndexHeader));
        write_index(big + row);
        std::string read;
        CHECK(dlpjoin::ReadIndexFile(&read));
        CHECK_EQ(read, dlpjoin::IndexAsPluginReads(big + row));
        CHECK(read.size() <= dc::kIndexMaxBytes && read.back() == '\n');
        Kids k;
        CHECK(dlpjoin::LoadChildren(present, plugin, k.ch.get(), &k.missing));
        CHECK(RowVerdict(k, kIslandChild) == dc::Verdict::NoRow);
        CHECK_EQ(k.missing.size(), 1u);  // "index not written": the plugin of the child would not find its row
        write_index(row.empty() ? big : std::string(dc::kIndexHeader) + "\n" + row + big.substr(sizeof(dc::kIndexHeader)));
        Kids k2;
        CHECK(dlpjoin::LoadChildren(present, plugin, k2.ch.get(), &k2.missing));
        CHECK(RowVerdict(k2, kIslandChild) == dc::Verdict::Ready);  // (the same row before the cut)
    }
    {
        std::remove((std::string("sdmc:") + dc::kIndexPath).c_str());
        std::string read = "x";
        CHECK(!dlpjoin::ReadIndexFile(&read));
        CHECK(read.empty());
    }
    CHECK(chdir(old) == 0);
    std::filesystem::remove_all(dir);
}

// J13 (the build 2f, B3): the line of the scans - the hosts, their child, the game of the row and its mode, the state, the members that wait; never
// a MAC or a name - and the log of a session: a line equal to the last is not kept, at most ScanLog::kLines, the rest counted
static void J13_ScanLine() {
    std::printf("J13: the line of the scans and its log (the build 2f, B3)\n");
    CHECK(dlpguest::Register(kIslandPrepared, dlpguest::kFormApplication));
    const std::string text = IndexText({IslandRow()});
    const std::vector<dlpguest::Guest> one = {IslandGuest()};
    Kids k;
    CHECK(Build(&text, one, Fakes(), &k));
    std::vector<dlpjoin::Host> all;
    dlpjoin::Host island{};
    island.g.host = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    island.g.child_tid_low = static_cast<u32>(kIslandChild);
    island.g.total_nodes = 1;
    island.g.max_nodes = 4;
    const char16_t* name = u"HostName";
    for (int i = 0; name[i] != 0 && i < 10; ++i) {
        island.g.name[i] = static_cast<u16>(name[i]);
    }
    island.v = dc::Verdict::Ready;
    island.prepared = kIslandPrepared;
    island.index_row = true;
    dlpjoin::Host other = island;
    other.g.child_tid_low = 0x00123456u;
    other.v = dc::Verdict::NoRow;
    other.prepared = 0;
    all.push_back(island);
    all.push_back(other);
    const std::string line = dlpjoin::ScanLine(all, *k.ch, 2);
    CHECK(line == "dlp: scan: 2 hosts: 000F8200 game 00040000000F8200 RealDlp ready 1/4; 00123456 another game no row 1/4; members waiting 2");
    CHECK(line.find("02:11") == std::string::npos && line.find("021122") == std::string::npos && line.find("HostName") == std::string::npos);
    all[0].full = true;
    CHECK(dlpjoin::ScanLine({all[0]}, *k.ch, 0) == "dlp: scan: 1 host: 000F8200 game 00040000000F8200 RealDlp full 1/4; members waiting 0");
    all[0].full = false;
    all[0].index_row = false;
    CHECK(dlpjoin::ScanLine({all[0]}, *k.ch, 0).find("grey index not written") != std::string::npos);
    all[0].index_row = true;
    all[0].v = dc::Verdict::OtherVersion;
    CHECK(dlpjoin::ScanLine({all[0]}, *k.ch, 0).find("grey another version") != std::string::npos);
    CHECK(dlpjoin::ScanLine({}, *k.ch, 0) == "dlp: scan: 0 hosts; members waiting 0");
    // the log: a line like the last one is not kept; each line has its ms from the first scan; the LAST kLines kept, the earlier ones counted
    // and said first (the review of the build 2f: the first ones were kept, with no time); Take empties it and starts the clock again
    dlpjoin::ScanLog log;
    log.Note("a", 1000);
    log.Note("a", 1100);
    log.Note("b", 1250);
    log.Note("a", 4000);
    CHECK(log.lines.size() == 3 && log.dropped == 0);
    std::vector<std::string> taken = log.Take();
    CHECK(taken.size() == 3 && taken[0] == "a (+0 ms)" && taken[1] == "b (+250 ms)" && taken[2] == "a (+3000 ms)" && log.lines.empty() && log.last.empty());
    for (std::size_t i = 0; i < dlpjoin::ScanLog::kLines + 5; ++i) {
        log.Note("line " + std::to_string(i), 500 + 10 * i);
    }
    CHECK(log.lines.size() == dlpjoin::ScanLog::kLines && log.dropped == 5);
    taken = log.Take();
    CHECK(taken.size() == dlpjoin::ScanLog::kLines + 1 && taken.front() == "dlp: scan: 5 earlier lines dropped (kept the last 32)" && log.dropped == 0);
    CHECK(taken[1] == "line 5 (+50 ms)" && taken.back() == "line 36 (+360 ms)"); // (the last ones: nearest the jump or the exit)
    CHECK(log.Take().empty());
    log.Note("c", 9000);
    CHECK(log.Take().front() == "c (+0 ms)");
    dlpguest::ClearRegistry();
}

int main() {
    int c = g_checks;
    J13_ScanLine();
    Ran("J13", c, 14);
    c = g_checks;
    J8_Logic();
    Ran("J8", c, 4200);
    c = g_checks;
    J11_Compose();
    Ran("J11", c, 3);
    c = g_checks;
    J12_EndNetParts();
    Ran("J12", c, 6);
    c = g_checks;
    J9_Table();
    Ran("J9", c, 40);
    c = g_checks;
    J1_IslandTour();
    Ran("J1/J10", c, 30);
    c = g_checks;
    J1b_ScanThenJoin();
    J1c_Look();
    J2_OtherVersion();
    J3_J4_HiddenAndFull();
    J4b_HostOf();
    Ran("J1b-J4b", c, 50);
    c = g_checks;
    J5_Leaves();
    Ran("J5", c, 12);
    c = g_checks;
    J6_HostGone();
    Ran("J6", c, 6);
    c = g_checks;
    J7_OurHost();
    Ran("J7", c, 60);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
