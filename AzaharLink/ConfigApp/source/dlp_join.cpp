#include "dlp_join.hpp"

#include <cstdio>
#include <ctime>
#include <new>

#include "dlp_guest_paths.hpp"
#include "fileio.hpp"
#include "heap_plan.hpp"

namespace dlpjoin {

namespace {

constexpr std::size_t kLogLines = 200;
constexpr std::size_t kLogBytes = 32u << 10;
constexpr const char* kLogName = "3DSLink-dlp-join.log";
constexpr std::size_t kSeen = 2 * azl::kMaxGroups;  // the hosts the list showed, kept for a join after a new scan cleared the lobby's table

azl::LobbyConfig ConfigOf(const azl::Mac& me, const std::u16string& nickname) {
    azl::LobbyConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.game_tid = azl::title_rules::kAppTid;
    cfg.friend_seed = FriendSeedOf(me);
    for (std::size_t k = 0; k < azl::kNameUnits && k < nickname.size(); ++k) {
        cfg.name[k] = static_cast<u16>(nickname[k]);
    }
    cfg.max_players = static_cast<u8>(azl::kMaxPlayers);  // (any game's lobby: the tables' ceiling, not a title's - the plugin's MakeClntHub)
    cfg.any_title = 1;
    return cfg;
}

}  // namespace

// ---- Core --------------------------------------------------------------------------------------------------------------------------------

Core::Core(azl::UdsEngine* engine, const azl::Mac& me, const std::u16string& nickname, const azl::dlp_children::Table* table,
           const std::vector<u64>& missing_row)
    : net_(engine), lobby_(&net_, ConfigOf(me, nickname)), missing_row_(missing_row) {
    const azl::LobbyConfig cfg = ConfigOf(me, nickname);
    azl::Identity spectator = {};
    spectator.seed = cfg.friend_seed;
    std::memcpy(spectator.name, cfg.name, sizeof(spectator.name));
    net_.SetIdentity(spectator);
    net_.SetPlayers(cfg.max_players);
    net_.SetClntBook(&book_);
    net_.SetChildren(table);
    static_assert(sizeof(seen_) / sizeof(seen_[0]) == kSeen, "the hosts the list showed: twice the lobby's table");
}

Core::~Core() {
    Wipe(joined_.data(), joined_.size());
    Wipe(ask_host_.data(), ask_host_.size());
}

void Core::MarkRelease() {
    release_mark_.store(rounds_.load() + kLeaveRounds);
    released_.store(true);
}

bool Core::Missing(u32 child_low) const {
    const u64 child = (static_cast<u64>(azl::title_rules::kTitleHighDlpChild) << 32) | child_low;
    for (const u64 c : missing_row_) {
        if (c == child) {
            return true;
        }
    }
    return false;
}

Host Core::HostOf(const azl::Group& g) {
    Host h;
    h.g = g;
    h.v = book_.VerdictOf(g);
    h.prepared = book_.PreparedFor(g);
    h.full = g.max_nodes != 0 && g.total_nodes >= g.max_nodes;
    h.index_row = !Missing(g.child_tid_low);
    h.block = book_.BlockFor(g);
    return h;
}

void Core::Step(u64 now_ms) {
    azl::platform::LockGuard<azl::platform::Mutex> lock(mu_);
    const u32 want = want_.exchange(0);
    if (!started_ && !ended_) {
        started_ = true;
        lobby_.ClientInitialize(nullptr, kMaxBeacons);
        lobby_.ClientStartScan(0);
    }
    if ((want & kWantEnd) != 0) {
        lobby_.ClientFinalize();  // (a probe, a join or a session end here: ScanClose / Disconnect, the Deauthentication into the room's queue)
        joining_ = false;
        ended_ = true;
        MarkRelease();
    } else if (!ended_) {
        if ((want & kWantLeave) != 0) {
            lobby_.ClientStopSession();
            joining_ = false;
            joined_prepared_ = 0;
            MarkRelease();
            lobby_.ClientStartScan(0);  // (the list again: the session stays in the room)
        }
        if ((want & kWantScan) != 0) {
            lobby_.ClientStopScan();
            lobby_.ClientStartScan(0);
        }
        if ((want & kWantJoin) != 0) {
            DoJoin((want & (kWantScan | kWantLeave)) != 0);
        }
    }
    net_.Pump();
    lobby_.Pump();
    if (joining_) {
        u32 r = 0;
        if (lobby_.ClientJoinPoll(&r)) {
            joining_ = false;
            if (r != 0) {
                last_result_ = r;
                joined_prepared_ = 0;
                lobby_.ClientStopSession();
                MarkRelease();
                lobby_.ClientStartScan(0);
            }
        }
    }
    if (lobby_.client_state() == azl::kCliComplete && passphrase_ms_ == 0) {
        passphrase_ms_ = now_ms != 0 ? now_ms : 1;
    }
    rounds_.fetch_add(1);
}

void Core::DoJoin(bool rescanned) {
    const u8 st = lobby_.client_state();
    const azl::Group* g = lobby_.ClientGroupOf(ask_host_);
    // (the list's copy only when this round began a new scan, which cleared the lobby's table: a host the scan itself dropped - full, gone - is refused)
    for (std::size_t i = 0; rescanned && g == nullptr && i < kSeen; ++i) {
        g = seen_used_[i] && seen_[i].host == ask_host_ ? &seen_[i] : nullptr;
    }
    if (g == nullptr || joining_ || (st != azl::kCliIdle && st != azl::kCliScanning)) {
        refusals_.fetch_add(1);
        return;
    }
    const azl::Group chosen = *g;
    const Host h = HostOf(chosen);
    if (ShowOf(h) != Show::Join) {
        refusals_.fetch_add(1);  // (not Ready, full, or not in the index file: nothing is sent)
        return;
    }
    // ClntStartSession's sequence (the plugin's dlp:CLNT): the lobby's join refuses the state 2 and a host its table does not hold
    if (st == azl::kCliScanning) {
        lobby_.ClientStopScan();
    }
    lobby_.ClientAdoptGroup(chosen);
    u32 res = 0;
    if (lobby_.ClientJoinBegin(chosen.host, &res) == azl::JoinStep::Pending) {
        joining_ = true;
        joined_ = chosen.host;
        joined_prepared_ = h.prepared;
    } else {
        last_result_ = res;
        lobby_.ClientStartScan(0);
    }
}

void Core::AskScan() {
    want_.fetch_or(kWantScan);
}

void Core::AskJoin(const azl::Mac& host) {
    {
        azl::platform::LockGuard<azl::platform::Mutex> lock(mu_);
        ask_host_ = host;
    }
    want_.fetch_or(kWantJoin);
}

void Core::AskLeave() {
    want_.fetch_or(kWantLeave);
}

void Core::AskEnd() {
    // (an earlier release - a B, a join that failed, TakeReboot - is forgotten BEFORE the intent is posted: Step takes the intent, then runs
    // ClientFinalize, which queues the Deauthentication, and only then marks ITS release; with the old mark still standing LeaveFlushed was true
    // in between, and RoomClient::Stop dropped the Deauthentication still being queued - D-B10)
    released_.store(false);
    want_.fetch_or(kWantEnd);
}

std::vector<Host> Core::Hosts() {
    std::vector<Host> out;
    azl::platform::LockGuard<azl::platform::Mutex> lock(mu_);
    for (u32 slot = 0; slot < azl::kMaxGroups; ++slot) {
        const azl::Group* g = lobby_.ClientScanEntry(slot);
        if (g == nullptr) {
            continue;
        }
        out.push_back(HostOf(*g));
        // (kept for AskJoin: its own entry, else a free one, else the oldest)
        std::size_t at = kSeen;
        for (std::size_t i = 0; i < kSeen && at == kSeen; ++i) {
            at = seen_used_[i] && seen_[i].host == g->host ? i : kSeen;
        }
        for (std::size_t i = 0; i < kSeen && at == kSeen; ++i) {
            at = !seen_used_[i] ? i : kSeen;
        }
        if (at == kSeen) {
            at = seen_next_;
            seen_next_ = (seen_next_ + 1) % kSeen;
        }
        seen_[at] = *g;
        seen_used_[at] = true;
    }
    return out;
}

u32 Core::Word() {
    azl::platform::LockGuard<azl::platform::Mutex> lock(mu_);
    return lobby_.ClientStatusWord();
}

bool Core::Joining() const {
    return joining_.load();
}

u32 Core::Rounds() const {
    return rounds_.load();
}

Core::View Core::Look() {
    azl::platform::LockGuard<azl::platform::Mutex> lock(mu_);  // (Step holds it for its whole round: the four are of one instant)
    View v;
    v.word = lobby_.ClientStatusWord();
    v.joining = joining_.load();
    v.rounds = rounds_.load();
    v.refusals = refusals_.load();
    return v;
}

bool Core::LeaveFlushed() const {
    return released_.load() && want_.load() == 0 && rounds_.load() >= release_mark_.load();
}

u32 Core::refusals() const {
    return refusals_.load();
}

u32 Core::last_join_result() const {
    return last_result_.load();
}

u64 Core::passphrase_ms() {
    azl::platform::LockGuard<azl::platform::Mutex> lock(mu_);
    return passphrase_ms_;
}

bool Core::TakeReboot(u8 (&info)[16], u64* prepared) {
    azl::platform::LockGuard<azl::platform::Mutex> lock(mu_);
    if (lobby_.client_state() != azl::kCliComplete || joined_prepared_ == 0 || lobby_.ClientGroupOf(joined_) == nullptr) {
        return false;
    }
    u8 mac[6];
    u8 pass[azl::kPassphraseSize];
    std::memcpy(mac, joined_.data(), sizeof(mac));
    lobby_.ClientPassphrase(pass);
    ComposeRebootInfo(mac, pass, info);
    Wipe(pass, sizeof(pass));
    Wipe(mac, sizeof(mac));
    *prepared = joined_prepared_;
    lobby_.ClientStopSession();  // (as dlplay: StopSession after the passphrase - the lobby forgets it, the host's network is let go)
    MarkRelease();
    Wipe(joined_.data(), joined_.size());
    joined_prepared_ = 0;
    return true;
}

Core* MakeCore(void* block, std::size_t size, azl::UdsEngine* engine, const azl::Mac& me, const std::u16string& nickname,
               const azl::dlp_children::Table* table, const std::vector<u64>& missing_row) {
    if (block == nullptr || size < sizeof(Core)) {
        return nullptr;
    }
    return new (block) Core(engine, me, nickname, table, missing_row);
}

void EndCore(Core* core) {
    if (core == nullptr) {
        return;
    }
    core->~Core();
    Wipe(core, sizeof(Core));  // (the block held the lobby: a passphrase, a host's MAC, the session's seed)
}

bool LeaveRounds(Core& core, u64 (*now_ms)(), void (*sleep_ms)(u32)) {
    core.AskEnd();
    const u64 until = now_ms() + kLeaveWaitMs;
    while (!core.LeaveFlushed()) {
        if (now_ms() >= until) {
            return false;
        }
        sleep_ms(2);
    }
    return true;
}

u64 HandOver(Core& core, s32 (*send)(const u8 (&info)[16]), s32* rc) {
    u8 info[16];
    u64 prepared = 0;
    if (!core.TakeReboot(info, &prepared)) {
        *rc = -1;
        return 0;
    }
    *rc = send(info);
    Wipe(info, sizeof(info));
    return *rc == 0 ? prepared : 0;
}

// ---- the files ---------------------------------------------------------------------------------------------------------------------------

bool ReadIndexFile(std::string* text) {
    text->clear();
    const std::string path = std::string("sdmc:") + azl::dlp_children::kIndexPath;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    // (one byte more than the plugin takes: it tells a longer file, which the plugin cuts at its last whole line)
    std::string whole(azl::dlp_children::kIndexMaxBytes + 1, '\0');
    const std::size_t got = std::fread(&whole[0], 1, whole.size(), f);
    std::fclose(f);
    whole.resize(got);
    *text = IndexAsPluginReads(whole);
    return !text->empty();
}

bool LoadChildren(const PresenceFn& present, const FileFn& plugin_there, Children* out, std::vector<u64>* missing_row) {
    std::string text;
    const bool read = ReadIndexFile(&text);
    const std::vector<dlpguest::Guest> guests = dlpguest::LoadGuests();
    const BlockFn block = [](u64 prepared, u8* buf) {
        FILE* f = std::fopen(dlpguest::BlockPath(prepared).c_str(), "rb");
        if (f == nullptr) {
            return false;
        }
        const std::size_t got = std::fread(buf, 1, azl::dlp_children::kBlockBytes, f);
        std::fclose(f);
        return got == azl::dlp_children::kBlockBytes;
    };
    return BuildTable(read ? text.data() : nullptr, text.size(), guests, present, plugin_there, block, out, missing_row);
}

void AppendLog(const std::string& datadir, const std::string& line) {
    if (datadir.empty()) {
        return;
    }
    const std::string path = datadir + "/" + kLogName;
    char stamp[96];
    if (heap_plan::FormatStamp(static_cast<int64_t>(std::time(nullptr)), static_cast<uint32_t>(azl::platform::NowMs()), stamp, sizeof(stamp)) <= 0) {
        stamp[0] = '\0';
    }
    std::string text;
    if (setup::ReadTextFile(path, kLogBytes * 2, text) == setup::LoadResult::Unreadable) {
        return;  // (a file that could not be read is not written over)
    }
    setup::WriteTextFile(path, heap_plan::KeepLast(text, std::string(stamp) + line, kLogLines, kLogBytes));
}

void AppendLogLines(const std::string& datadir, const std::vector<std::string>& lines) {
    if (datadir.empty() || lines.empty()) {
        return;
    }
    const std::string path = datadir + "/" + kLogName;
    char stamp[96];
    if (heap_plan::FormatStamp(static_cast<int64_t>(std::time(nullptr)), static_cast<uint32_t>(azl::platform::NowMs()), stamp, sizeof(stamp)) <= 0) {
        stamp[0] = '\0';
    }
    std::string text;
    if (setup::ReadTextFile(path, kLogBytes * 2, text) == setup::LoadResult::Unreadable) {
        return;  // (a file that could not be read is not written over)
    }
    for (const std::string& line : lines) {
        text = heap_plan::KeepLast(text, std::string(stamp) + line, kLogLines, kLogBytes);
    }
    setup::WriteTextFile(path, text);
}

}  // namespace dlpjoin

// ---- the console's side ------------------------------------------------------------------------------------------------------------------
#ifdef __3DS__
#include <sys/stat.h>

#include "azl/room_members.hpp"
#include "dlp_guest_am.hpp"
#include "dlp_join_sys.hpp"
#include "games.hpp"
#include "zombie_free.hpp"

namespace dlpjoin {

namespace {

/// The player's name as the room has it (UTF-8) in UTF-16 units: what the lobby says of us.
std::u16string Utf16Of(const std::string& s) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
        const u8 c = static_cast<u8>(s[i]);
        u32 cp = 0xFFFD;
        std::size_t n = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<u8>(s[i + 1]) & 0x3Fu);
            n = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<u8>(s[i + 1]) & 0x3Fu) << 6) | (static_cast<u8>(s[i + 2]) & 0x3Fu);
            n = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            cp = ((c & 0x07u) << 18) | ((static_cast<u8>(s[i + 1]) & 0x3Fu) << 12) | ((static_cast<u8>(s[i + 2]) & 0x3Fu) << 6) |
                 (static_cast<u8>(s[i + 3]) & 0x3Fu);
            n = 4;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
        i += n;
    }
    return out;
}

// (the build 2h) the plugin in the folder of the PROCESS's id of the registered form (ProcessIdOf: the system id 00040010|low, or the key)
bool PluginThere(u64 prepared) {
    struct stat st;
    return stat(setup::GamePluginPath(dlpguest::ProcessIdOf(prepared)).c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

}  // namespace

Session::~Session() {
    Stop();
}

void Session::FreeNet(Net* n) {
    if (n->core != nullptr) {
        EndCore(n->core);
    }
    ::operator delete(n->core_block);
    delete n;  // (the engine and the table with it; the room client's own Stop finds no thread)
}

Why Session::Start(const Facts& f, const SessionIn& in) {
    Stop();
    const Why why = JoinGate(f);
    if (why != Why::Ok) {
        return why;
    }
    datadir_ = in.datadir;
    Net* const n = new (std::nothrow) Net;  // (what the callbacks below capture: never `this`, which goes with the page)
    if (n == nullptr) {
        return Why::NoGuests;
    }
    n->children.reset(new (std::nothrow) Children);
    if (!n->children) {
        FreeNet(n);
        return Why::NoGuests;
    }
    // (DPAPP-2j) a child in the system form of the build 2h (no console has it; Azahar falls on it) is never joined: "not prepared" until Update.
    // The form M3c1 is joined: its closed game is freed by 3DS Link (zombie_free.cpp) and the gate at A refuses while one is in memory
    LoadChildren([](u64 prepared, u16* version) {
                     return dlpguest::Jumpable(dlpguest::FormOf(prepared)) ? dlpguest::PreparedPresence(prepared, version) : dlpguest::Presence::Absent;
                 },
                 &PluginThere, n->children.get(), &missing_);
    if (ReadyCount(n->children->table) == 0) {
        Log("dlp: the page found no child ready in the index file and the app's records (nothing joined)");
        FreeNet(n);
        missing_.clear();
        return Why::NoGuests;
    }
    azl::UdsEngine::Transport transport;
    transport.send = [n](azl::WifiPacket& packet) {
        if (!n->room.joined()) {
            return;  // (not in the room: frames go nowhere)
        }
        packet.transmitter_address = n->room.mac();
        n->room.SendWifiPacket(packet);
    };
    const azl::Mac console = in.mac;
    transport.get_mac = [n, console]() { return n->room.joined() ? n->room.mac() : console; };
    n->engine.reset(new (std::nothrow) azl::UdsEngine(&n->kernel, transport));
    n->core_block = n->engine ? ::operator new(sizeof(Core), std::nothrow) : nullptr;
    n->core = MakeCore(n->core_block, sizeof(Core), n->engine.get(), in.mac, Utf16Of(in.nickname), &n->children->table, missing_);
    if (n->core == nullptr) {
        FreeNet(n);
        missing_.clear();
        return Why::NoGuests;
    }
    net_ = n;
    n->room.SetWifiCallback([n](const azl::WifiPacket& packet) { n->engine->OnWifiPacket(packet); });
    n->room.SetWifiPrefilter([n](const u8* message, std::size_t size) { return n->engine->TryDiscardEncoded(message, size); });
    n->room.SetTickCallback([n](u64 now_ms) {
        n->engine->Tick(now_ms);
        n->core->Step(now_ms);
    });
    n->room.SetRoomInfoCallback([n](const azl::room::RoomInfo& info) {
        std::vector<azl::Mac> plays;
        std::vector<Member> waiting;
        for (const azl::room::MemberInfo& m : info.members) {
            if (azl::RoomMemberPlays(m.game_name, m.game_id)) {
                plays.push_back(m.mac);
            }
            const int r = n->children ? azl::dlp_children::FindPrepared(n->children->table.index, azl::title_rules::PreparedOf(m.game_id)) : -1;
            if (r >= 0 && n->children->table.verdict[r] == static_cast<u8>(azl::dlp_children::Verdict::Ready)) {
                waiting.push_back({m.nickname, m.game_id, m.mac});
            }
        }
        n->engine->OnRoomMembers(plays);
        azl::platform::LockGuard<azl::platform::Mutex> lock(n->members_mu);
        n->members.swap(waiting);
    });
    azl::RoomClient::Config rc;
    rc.host = in.host;
    rc.port = in.port;
    rc.nickname = in.nickname;
    rc.password = in.password;
    rc.preferred_mac = in.mac;  // (D-B4: the console's own MAC - the child comes back with it)
    char id[40];
    std::snprintf(id, sizeof(id), "3ds-%02x%02x%02x%02x%02x%02x-dlp", in.mac[0], in.mac[1], in.mac[2], in.mac[3], in.mac[4], in.mac[5]);
    rc.console_id_hash = id;  // (D-B4: not the plugin's "3ds-<mac>": only the console id collision is avoided)
    rc.game_name = "Download Play (3DS Link)";
    rc.game_id = azl::title_rules::kAppTid;
    if (!n->room.Start(rc)) {
        Stop();
        return Why::NoRoom;
    }
    Log("room: the Download Play page joins the room (" + std::to_string(ReadyCount(n->children->table)) + " children ready, " +
        std::to_string(missing_.size()) + " not in the index file)");
    return Why::Ok;
}

void Session::Stop() {
    std::vector<std::string> lines;
    StopInto(&lines);
    AppendLogLines(datadir_, lines); // (ONE write of the log: the scans and the leaving)
}

// (the review of the build 2f) the end of the session, its lines given to the caller - the scans of the session (B3), then the leaving - so that
// they go to the log in ONE write: Stop's, or HandOverAndLeave's together with its line (on the way out of the jump, one write as before B3)
void Session::StopInto(std::vector<std::string>* lines) {
    Net* const n = net_;
    if (n == nullptr) {
        return;  // (twice is nothing)
    }
    net_ = nullptr;
    const bool rounds = LeaveRounds(*n->core, &azl::platform::NowMs, &azl::platform::SleepMs);
    *lines = scan_log_.Take(); // (the build 2f, B3: the scans of the session, after the room is left - they came before the leaving)
    const bool freed = EndNetParts(
        [this, n, rounds, lines] {
            const bool ended = n->room.Stop();
            left_ms_ = azl::platform::NowMs();
            left_ack_ = n->room.LeaveAcknowledged();
            last_error_ = n->room.error();
            lines->push_back(std::string("room: left (") + (left_ack_ ? "the room answered" : "no answer in 500 ms: it may list this console a few seconds") +
                             (rounds ? ")" : "; the network thread did not run two rounds in 150 ms)"));
            return ended;
        },
        [n] { FreeNet(n); });
    if (!freed) {
        lines->push_back("room: the network thread did not stop in 2 s: the session's memory is kept for it, never freed under it");
    }
    missing_.clear();
}

u64 Session::HandOverAndLeave(s32* rc) {
    if (net_ == nullptr) {
        *rc = -1;
        return 0;
    }
    const u64 pass_ms = net_->core->passphrase_ms();
    const u64 prepared = HandOver(*net_->core, &SetWirelessRebootInfo, rc);
    std::vector<std::string> lines;
    StopInto(&lines); // (the review of the build 2f: its lines go with the jump's below, in one write)
    char line[160];
    if (prepared != 0) {
        std::snprintf(line, sizeof(line), "dlp: the 16 bytes went to ns:s; from the passphrase to the room exit %lu ms (the host waits 5000 ms at most)",
                      static_cast<unsigned long>(pass_ms != 0 && left_ms_ > pass_ms ? left_ms_ - pass_ms : 0));
    } else {
        std::snprintf(line, sizeof(line), "dlp: no jump: %s (0x%08lx)", *rc == -1 ? "the lobby was not in its state 9" : "ns:s refused the 16 bytes",
                      static_cast<unsigned long>(*rc));
    }
    lines.push_back(line);
    AppendLogLines(datadir_, lines);
    return prepared;
}

Phase Session::phase(Core::View* view) {
    if (net_ == nullptr) {
        if (view != nullptr) {
            *view = Core::View();
        }
        return Phase::Off;
    }
    const Core::View v = net_->core->Look();  // (one look: the word and the join flag of the same Step - review of B4)
    if (view != nullptr) {
        *view = v;
    }
    return PhaseOf(v.word, v.joining, net_->room.state());
}

std::vector<Member> Session::WaitingMembers(const std::vector<Host>& hosts) {
    std::vector<Member> out;
    if (net_ == nullptr) {
        return out;
    }
    azl::platform::LockGuard<azl::platform::Mutex> lock(net_->members_mu);
    for (const Member& m : net_->members) {
        if (!HostsALobby(hosts, m.mac)) {  // (its lobby is in the list already: by MAC, the room's nickname is not the beacon's name)
            out.push_back(m);
        }
    }
    return out;
}

void Session::Log(const std::string& line) {
    AppendLog(datadir_, line);
}

bool Session::ClosedGameLeft(u64 prepared, bool* unread) {
    u32 ms = 0;
    const bool left = zombie_free::Remaining(prepared, &ms, unread);
    char line[200];
    if (*unread) {
        // (DPAPP-2j-rev) no closed game seen: the list of the processes was not read whole (twice) - said as it is, A tries again
        std::snprintf(line, sizeof(line), "dlp: gate: the list of the processes not read whole for %016llX (%lu ms): join refused, A tries again",
                      static_cast<unsigned long long>(prepared), static_cast<unsigned long>(ms));
    } else {
        std::snprintf(line, sizeof(line), "dlp: gate: %s of %016llX in memory (%lu ms)%s", left ? "a closed game" : "no closed game",
                      static_cast<unsigned long long>(prepared), static_cast<unsigned long>(ms), left ? ": join refused (327)" : "");
    }
    Log(line);
    return left;
}

}  // namespace dlpjoin
#endif
