// The book of dlp:CLNT (1.1.4, G1 of analysis/piano-1.1.4-2026-11-15/specifiche/1-G1.md section 1.2, with the contract K3, K4 and K7 of 7-CONTRATTO.md):
// what the client of the SYSTEM's Download Play application needs beside the lobby, which keeps no more than a game's client does.
//   - The RECORDS delivered to the application (GetTitleInfoInOrder): up to kRecords hosts, kept until Initialize / Finalize, so that GetServerInfo
//     never fails on a host the application was told about - it reads it again at every frame it draws and before the jump, and does not look at the
//     result. A scan marks what it delivered (NewScan clears the marks, never the records); a seventeenth host is refused, never one known evicted.
//   - The TITLES of the hosts heard (frame 0 of their announcement, read on the network thread by the searcher of the real Download Play, at frame 0): the short name a host
//     announces (big-endian units) and the two sizes at +0x38 / +0x3C, which tell the child a host offers from another version of it (K3: G2.2 is
//     not needed, the book keeps them).
//   - The CHILDREN: the table of the children the app prepared (azl/dlp_children.hpp) is the adapter's of the real Download Play (SetChildren, an atomic pointer published by
//     the plugin's thread); the book reads it through the pointer it is given (SetChildrenSource) and asks the plugin's thread for the block of a
//     child only when a host it heard announces that child (wanted_: the plugin's thread reads it, one block at a time, never on a game's thread).
//   - GetMyStatus: the state 0x80 forced (a refused StartSession, HOME), and the work deferred while no network thread runs (the first touch of the
//     application comes before the room is joined: 4.6 s against 5.7 s on a console).
//   - The seam to G3 (K7): the prepared title of the child of the session the application joined (armed_prepared, from the 'prepared' field of the
//     row of the index - never computed from the child) and whether the passphrase came (passphrase_ready). The plugin's thread reads them
//     (Plugin::Tick in plugin.cpp, with or without the room's link); this piece only exposes them.
// THREADS. Everything but the atomics is under DlpHub::dlp_mutex_: the commands served inline take it, the network thread holds it in Tick (the
// adapter's Pump, Collect's NoteTitle, the posted commands). wanted_, armed_low_, passphrase_, refusals_ and the children's pointer are read without
// it by the plugin's thread. Pure header: no heap, no lock, no log, no kernel.
#pragma once

#include <atomic>

#include "azl/common.hpp"
#include "azl/dlp_children.hpp"   // (the table of the children, the verdicts, the small line and title_long)
#include "azl/dlp_clnt_map.hpp"
#include "azl/dlp_lobby.hpp"      // (Group, Mac, kMaxGroups)
#include "azl/dlp_title_block.hpp"
#include "azl/dlp_types.hpp"

namespace azl {

// >>> game-thread code (Tests/policy_check.sh: the book is read and written on the Download Play application's thread, under the hub's mutex)

class DlpClntBook {
public:
    /// The records the application keeps at most (its code keeps 16 entries); the children of the index are as many (dlp_children::kMaxRows).
    static constexpr u32 kRecords = 16;
    /// The hosts whose title is kept: what a scan probes (UdsEngine::kHeardLobbies = the lobby's kMaxGroups).
    static constexpr u32 kTitles = kMaxGroups;
    static constexpr u32 kTitleShortBytes = static_cast<u32>(dlp_real::kTitleShortBytes);
    // Frame 0 of an announcement (azl/dlp_real_wire.hpp, AnnounceFrame): +0x38 transfer size, +0x3C required size (big-endian), +0x40 the short name.
    static constexpr u32 kFrame0TransferAt = 0x38;
    static constexpr u32 kFrame0RequiredAt = 0x3C;
    static constexpr u32 kFrame0TitleAt = 0x40;
    static constexpr u32 kFrame0Least = kFrame0TitleAt + kTitleShortBytes;  // 0xC0: shorter frames give nothing
    /// The work a posted command leaves when no network thread serves it (DlpHub::AnswerUnservedClnt): what the next Tick does (TakeDeferred).
    static constexpr u8 kDeferNone = 0, kDeferIdle = 1, kDeferScan = 2, kDeferStop = 3, kDeferStopSession = 4, kDeferFinal = 5;
    struct Title {
        Mac host;
        u8 used;
        u8 stamp;       // title_clock_ when it was noted: the oldest is replaced
        u32 transfer;   // frame 0 +0x38
        u32 required;   // frame 0 +0x3C
        u8 short_be[kTitleShortBytes];
    };

    DlpClntBook() {
        Reset();
    }

    // ---- the records delivered (GetTitleInfoInOrder), answered by GetServerInfo until Initialize / Finalize ----------------------------------
    /// Initialize, Finalize: records, marks, titles, the forced state, the deferred work and the seam to G3. Not the children's source.
    void Reset() {
        std::memset(rec_, 0, sizeof(rec_));
        std::memset(sizes_, 0, sizeof(sizes_));
        std::memset(titles_, 0, sizeof(titles_));
        used_ = 0;
        this_scan_ = 0;
        override_ = 0;
        defer_ = kDeferNone;
        beacons_ = kMaxGroups;
        title_clock_ = 0;
        Disarm();
    }
    /// StartScan (and GetTitleInfoInOrder with restart): every record may be delivered again, and the state 0x80 forced is lifted.
    void NewScan() {
        this_scan_ = 0;
        override_ = 0;
    }
    bool DeliveredThisScan(const Mac& host) const {
        const int r = IndexOf(host);
        return r >= 0 && ((this_scan_ >> r) & 1u) != 0;
    }
    /// Delivered now: a known host takes the record `g` gives, a new one the first free record. false when the 16 are taken and `g` is none of
    /// them: NOTHING known is evicted (the application would lose a host it shows).
    __attribute__((noinline)) bool Deliver(const Group& g) {
        int r = IndexOf(g.host);
        if (r < 0) {
            if (used_ >= kRecords) {
                return false;
            }
            r = static_cast<int>(used_++);
            KeepSizes(r, TitleOf(g.host));
        }
        rec_[r] = g;
        this_scan_ = static_cast<u16>(this_scan_ | (1u << r));
        return true;
    }
    /// A host the lobby still lists: its record takes the live total, signal, ceiling and name. An unknown host: nothing.
    __attribute__((noinline)) void Refresh(const Group& g) {
        const int r = IndexOf(g.host);
        if (r >= 0) {
            rec_[r] = g;
        }
    }
    const Group* Find(const Mac& host) const {
        const int r = IndexOf(host);
        return r >= 0 ? &rec_[r] : nullptr;
    }
    u32 records() const {
        return used_;
    }

    // ---- the titles (the network thread: the adapter of the real Download Play, at frame 0) ------------------------------------------------------------------
    /// Frame 0 of the announcement of `host`, whose child is 00040001`child_low`: its sizes and short name (len >= kFrame0Least, else nothing);
    /// the host's own slot, else a free one, else the oldest. The block of that child is asked of the plugin's thread if it was not read yet.
    __attribute__((noinline)) void NoteTitle(const Mac& host, u32 child_low, const u8* frame0, u32 len) {
        if (frame0 == nullptr || len < kFrame0Least) {
            return;
        }
        Title* t = nullptr;
        for (u32 i = 0; i < kTitles && t == nullptr; ++i) {
            t = titles_[i].used != 0 && titles_[i].host == host ? &titles_[i] : nullptr;
        }
        for (u32 i = 0; i < kTitles && t == nullptr; ++i) {
            t = titles_[i].used == 0 ? &titles_[i] : nullptr;
        }
        if (t == nullptr) {
            t = &titles_[0];
            for (u32 i = 1; i < kTitles; ++i) {
                if (static_cast<u8>(title_clock_ - titles_[i].stamp) > static_cast<u8>(title_clock_ - t->stamp)) {
                    t = &titles_[i];
                }
            }
        }
        t->host = host;
        t->used = 1;
        t->stamp = title_clock_++;
        t->transfer = LoadBE32(frame0 + kFrame0TransferAt);
        t->required = LoadBE32(frame0 + kFrame0RequiredAt);
        std::memcpy(t->short_be, frame0 + kFrame0TitleAt, kTitleShortBytes);
        KeepSizes(IndexOf(host), t);
        WantBlockOf(ChildOf(child_low));
    }
    const Title* TitleOf(const Mac& host) const {
        for (u32 i = 0; i < kTitles; ++i) {
            if (titles_[i].used != 0 && titles_[i].host == host) {
                return &titles_[i];
            }
        }
        return nullptr;
    }

    // ---- the children (K3) ---------------------------------------------------------------------------------------------------------------
    /// Where the table is published (the atomic pointer of the adapter of the real Download Play): given once, by its SetClntBook, before the first command.
    void SetChildrenSource(const std::atomic<const dlp_children::Table*>* source) {
        source_ = source;
    }
    const dlp_children::Table* children() const {
        return source_ != nullptr ? source_->load(std::memory_order_acquire) : nullptr;
    }
    static u64 ChildOf(u32 child_low) {
        return (static_cast<u64>(title_rules::kTitleHighDlpChild) << 32) | child_low;
    }
    /// The verdict on the child of host `g` (dlp_children::VerdictFor with the sizes its frame 0 announced, 0 if not heard). A row whose block
    /// was not read yet is Unknown, and the block is asked of the plugin's thread. The sizes of a host the book has a record of are the record's
    /// too: its title's slot (one of kTitles) may have gone to a later host, and "not heard" would make another version Ready (the final review).
    __attribute__((noinline)) dlp_children::Verdict VerdictOf(const Group& g) {
        const Title* t = TitleOf(g.host);
        const int r = t == nullptr ? IndexOf(g.host) : -1;
        const u32 transfer = t != nullptr ? t->transfer : (r >= 0 ? sizes_[r][0] : 0);
        const u32 required = t != nullptr ? t->required : (r >= 0 ? sizes_[r][1] : 0);
        const u64 child = ChildOf(g.child_tid_low);
        const dlp_children::Verdict v = dlp_children::VerdictFor(children(), child, transfer, required);
        if (v == dlp_children::Verdict::Unknown) {
            WantBlockOf(child);
        }
        return v;
    }
    /// The block of the child of host `g`, or nullptr (no table, no row, not read, none).
    __attribute__((noinline)) const u8* BlockFor(const Group& g) const {
        const dlp_children::Table* table = children();
        const int r = table != nullptr ? dlp_children::FindRow(table->index, ChildOf(g.child_tid_low)) : -1;
        return r >= 0 ? dlp_children::BlockOf(table, static_cast<std::size_t>(r)) : nullptr;
    }
    /// The prepared title of the row of the child of host `g` (its 'prepared' field: never computed from the child), or 0.
    __attribute__((noinline)) u64 PreparedFor(const Group& g) const {
        const dlp_children::Table* table = children();
        const int r = table != nullptr ? dlp_children::FindRow(table->index, ChildOf(g.child_tid_low)) : -1;
        return r >= 0 ? table->index.rows[r].prepared : 0;
    }
    /// The plugin's thread: the rows whose block was asked for (bit = row), taken.
    u32 TakeWantedBlocks() {
        return wanted_.exchange(0, std::memory_order_acq_rel);
    }
    /// ... and what it gives back when it reads fewer than it took (one block per round).
    void WantRows(u32 rows) {
        wanted_.fetch_or(rows, std::memory_order_acq_rel);
    }

    // ---- GetMyStatus --------------------------------------------------------------------------------------------------------------------
    /// A StartSession refused, HOME in a session: the application sees 0x80 ("the link was lost") until StartScan, StopSession or Initialize.
    void ForceLost() {
        override_ = kCliLost;
        Disarm();
    }
    void ClearOverride() {
        override_ = 0;
    }
    /// The word GetMyStatus gives: 0x80 forced; while work is deferred, scanning (2) for a StartScan and idle (1) for the rest; else the lobby's
    /// word with its state 6 shown as 7 (clnt::ShownWord).
    u32 Shown(u32 lobby_word) const {
        if (override_ != 0) {
            return static_cast<u32>(override_) << 24;
        }
        if (defer_ != kDeferNone) {
            return (defer_ == kDeferScan ? static_cast<u32>(kCliScanning) : static_cast<u32>(kCliIdle)) << 24;
        }
        return clnt::ShownWord(lobby_word);
    }

    // ---- no network thread (DlpHub::AnswerUnservedClnt): a pure state machine --------------------------------------------------------------
    /// A posted command that no network thread ran: true = deferred (*result = 0, the next Tick does what it leaves); false = it cannot wait
    /// (StartSession and every other id: *result = the module's "wireless off").
    bool Defer(u32 id, u32 beacons, u32* result) {
        switch (id) {
        case clnt::kInitialize:
            Reset();
            defer_ = kDeferIdle;
            beacons_ = static_cast<u8>(beacons);
            break;
        case clnt::kStartScan:
            NewScan();
            defer_ = kDeferScan;
            break;
        case clnt::kStopScan:
            defer_ = defer_ == kDeferNone ? kDeferStop : (defer_ == kDeferScan ? kDeferIdle : defer_);
            break;
        case clnt::kStopSession:
            ClearOverride();
            defer_ = (defer_ == kDeferNone || defer_ == kDeferStop) ? kDeferStopSession : defer_;
            break;
        case clnt::kFinalize:
            Reset();
            defer_ = kDeferFinal;
            break;
        default:
            *result = kWirelessOff;
            return false;
        }
        *result = 0;
        return true;
    }
    /// The network thread: the work deferred (and the beacons of a deferred Initialize), taken.
    u8 TakeDeferred(u8* beacons) {
        const u8 d = defer_;
        *beacons = beacons_;
        defer_ = kDeferNone;
        return d;
    }
    u8 deferred() const {
        return defer_;
    }
    /// The module's "wireless off" (DlpHub::kResultWirelessOff): the answer of a command that cannot wait for the network.
    static constexpr u32 kWirelessOff = 0xC940AC04;

    // ---- the seam to G3 (K7): read by the room's tick callback in plugin.cpp, without the hub's mutex --------------------------------------
    /// The application joined a host whose child is ready: the prepared title of its row (00040000 + `prepared`'s low word; 0 = disarmed).
    void Arm(u64 prepared) {
        armed_low_.store(static_cast<u32>(prepared), std::memory_order_release);
        passphrase_.store(false, std::memory_order_release);
    }
    void Disarm() {
        armed_low_.store(0, std::memory_order_release);
        passphrase_.store(false, std::memory_order_release);
    }
    /// The state 9 was shown while armed: the passphrase of the session is there.
    void NotePassphrase() {
        if (armed_low_.load(std::memory_order_acquire) != 0) {
            passphrase_.store(true, std::memory_order_release);
        }
    }
    u64 armed_prepared() const {
        const u32 low = armed_low_.load(std::memory_order_acquire);
        return low != 0 ? ((static_cast<u64>(title_rules::kTitleHighApplication) << 32) | low) : 0;
    }
    bool passphrase_ready() const {
        return passphrase_.load(std::memory_order_acquire);
    }
    /// A StartSession refused because the child is not ready (the plugin's thread posts the notice): how many so far, and the last verdict.
    void NoteRefusal(dlp_children::Verdict v) {
        refused_verdict_.store(static_cast<u8>(v), std::memory_order_relaxed);
        refusals_.fetch_add(1, std::memory_order_release);
    }
    u32 refusals() const {
        return refusals_.load(std::memory_order_acquire);
    }
    dlp_children::Verdict refused_verdict() const {
        return static_cast<dlp_children::Verdict>(refused_verdict_.load(std::memory_order_relaxed));
    }

private:
    int IndexOf(const Mac& host) const {
        for (u32 i = 0; i < used_ && i < kRecords; ++i) {
            if (rec_[i].host == host) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
    /// Record `r` (-1: none) keeps the two sizes of title `t` (nullptr: nothing heard, nothing kept).
    void KeepSizes(int r, const Title* t) {
        if (r >= 0 && t != nullptr) {
            sizes_[r][0] = t->transfer;
            sizes_[r][1] = t->required;
        }
    }
    /// The row of `child` has no block read yet: the plugin's thread is asked for it.
    void WantBlockOf(u64 child) {
        const dlp_children::Table* table = children();
        const int r = table != nullptr ? dlp_children::FindRow(table->index, child) : -1;
        if (r >= 0 && dlp_children::BlockStateOf(table, static_cast<std::size_t>(r)) == dlp_children::BlockState::NotRead) {
            wanted_.fetch_or(1u << r, std::memory_order_acq_rel);
        }
    }

    Group rec_[kRecords];   // 640 B
    u32 sizes_[kRecords][2]; // 128 B: the sizes (+0x38, +0x3C) the record's host announced last, kept when its title's slot goes to another host
    Title titles_[kTitles]; // 4 x 144 B
    const std::atomic<const dlp_children::Table*>* source_ = nullptr;
    std::atomic<u32> wanted_{0};
    std::atomic<u32> armed_low_{0};
    std::atomic<u32> refusals_{0};
    std::atomic<bool> passphrase_{false};
    std::atomic<u8> refused_verdict_{0};
    u16 used_ = 0;
    u16 this_scan_ = 0;     // bit r: record r was delivered in this scan
    u8 override_ = 0;       // 0 or kCliLost
    u8 defer_ = kDeferNone;
    u8 beacons_ = kMaxGroups;
    u8 title_clock_ = 0;
};
static_assert(DlpClntBook::kRecords == dlp_children::kMaxRows, "the book keeps as many records as the index has children (K2)");
static_assert(DlpClntBook::kRecords <= 16, "a scan's marks are the 16 bits of this_scan_");
static_assert(sizeof(DlpClntBook) <= 1400, "the book of dlp:CLNT: 16 records and their sizes, 4 titles and a few words");

/// DLPTitleInfo (kTitleInfoSize) for dlp:CLNT ONLY (DlpService::WriteTitleInfo stays the games', byte for byte), in the application's buffer (K4):
/// zeros; the child's unique id and variation; the host's MAC; title_short from frame 0 (big-endian units turned) or, with none heard, the child's
/// eight hex digits; then the block's names, icon and size (dlp_children::WriteFromBlock) if the block is there; then always title_long
/// (dlp_children::WriteTitleLong: "Hosted by <name> - <what the verdict says>"). Nothing is copied on the stack. Out of line: the handlers' frames stay small.
__attribute__((noinline)) inline void WriteClntTitleInfo(u8* dst, const Group& g, const DlpClntBook::Title* title, const u8* block,
                                                         dlp_children::Verdict v) {
    std::memset(dst, 0, kTitleInfoSize);
    StoreLE32(dst + kTitleInfoUniqueId, g.child_tid_low >> 8);
    StoreLE32(dst + kTitleInfoVariation, g.child_tid_low & 0xFF);
    std::memcpy(dst + kTitleInfoMac, g.host.data(), 6);
    if (title != nullptr) {
        for (u32 i = 0; i < DlpClntBook::kTitleShortBytes; i += 2) {
            dst[kTitleInfoTitleShort + i] = title->short_be[i + 1];  // (big-endian units on the wire, little-endian in the record)
            dst[kTitleInfoTitleShort + i + 1] = title->short_be[i];
        }
    } else {
        static constexpr char kDigits[] = "0123456789ABCDEF";
        for (u32 i = 0; i < 8; ++i) {
            StoreLE16(dst + kTitleInfoTitleShort + 2 * i, static_cast<u8>(kDigits[(g.child_tid_low >> (28 - 4 * i)) & 0xF]));
        }
    }
    if (block != nullptr) {
        dlp_children::WriteFromBlock(dst, block);
    }
    dlp_children::WriteTitleLong(dst, g.name, v);
}

/// DLPServerInfo (kServerInfoSize) for dlp:CLNT ONLY: zeros; +0 MAC; +7 signal; +8 max_nodes and +9 min(total, max_nodes) (both with the host, as the
/// games' writer); +0x1C the small line under the host's name (dlp_children::WriteLabel: "Name OK" / "Name NO", the name as it is for Unknown);
/// +0x290 `tick`. The other 15 node_info stay zero. Out of line, as the one above.
__attribute__((noinline)) inline void WriteClntServerInfo(u8* dst, const Group& g, dlp_children::Verdict v, u64 tick) {
    std::memset(dst, 0, kServerInfoSize);
    std::memcpy(dst + kServerInfoMac, g.host.data(), 6);
    dst[kServerInfoSignal] = g.signal;
    dst[kServerInfoMaxClients] = g.max_nodes;
    dst[kServerInfoJoined] = g.total_nodes < g.max_nodes ? g.total_nodes : g.max_nodes;
    dlp_children::WriteLabel(dst, g.name, v);
    StoreLE64(dst + kServerInfoTick, tick);
}

// <<< game-thread code

}  // namespace azl
