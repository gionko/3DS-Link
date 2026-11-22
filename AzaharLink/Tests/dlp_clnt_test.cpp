// The map of dlp:CLNT (1.1.4, G1 section 1.1 of analysis/piano-1.1.4-2026-11-15/specifiche/1-G1.md: azl/dlp_clnt_map.hpp), on a PC, pure:
// C1 every id 0..0x16 against the table of the spec (its dlp:FKCL twin, inline or posted, called by the Download Play application, MAC in the
// request); C2 the room's Download Play only for a plugin that joins a room by itself (the eight cases, Sources/main.cpp's formula); C4 the
// state GetMyStatus shows. The small line under a host's name is azl/dlp_children.hpp's (Tests/dlp_children_test.cpp).
// 1.1.4, G1: also the book of dlp:CLNT (azl/dlp_clnt_book.hpp): C5 the records, the titles of frame 0, the children's table and the seam to G3;
// C6 the work deferred with no network thread against the table of the spec (section 1.2); C7 the two records of dlp:CLNT byte for byte.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "azl/dlp_clnt_book.hpp"
#include "azl/dlp_clnt_map.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failures;                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)

using azl::u32;
using azl::u8;
namespace cl = azl::clnt;

// The table of 1-G1.md section 3, by hand: id, FKCL twin, inline, used by the Download Play application, MAC in the request.
struct Want {
    u32 id;
    u32 fkcl;
    bool inline_;
    bool used;
    bool mac;
};
const Want kWant[] = {
    {0x00, cl::kNoFkcl, true, false, false},  // (no such command: answered with an error, inline)
    {0x01, 0x01, false, true, false},         // Initialize
    {0x02, 0x02, false, true, false},         // Finalize
    {0x03, 0x03, true, false, false},         // GetEventDescription
    {0x04, 0x04, true, true, false},          // GetChannels
    {0x05, 0x05, false, true, false},         // StartScan
    {0x06, 0x06, false, true, false},         // StopScan
    {0x07, 0x07, true, true, true},           // GetServerInfo
    {0x08, 0x08, true, false, true},          // GetTitleInfo
    {0x09, 0x09, true, true, false},          // GetTitleInfoInOrder
    {0x0A, 0x0A, true, false, true},          // DeleteScanInfo
    {0x0B, cl::kNoFkcl, true, true, true},    // PrepareForSystemDownload
    {0x0C, cl::kNoFkcl, true, true, false},   // StartSystemDownload
    {0x0D, 0x0B, false, true, true},          // StartSession
    {0x0E, 0x0C, true, true, false},          // GetMyStatus
    {0x0F, 0x0D, true, false, false},         // GetConnectingNodes
    {0x10, 0x0E, true, false, false},         // GetNodeInfo
    {0x11, 0x0F, true, true, false},          // GetWirelessRebootPassphrase
    {0x12, 0x10, false, true, false},         // StopSession
    {0x13, cl::kNoFkcl, true, true, true},    // GetCupVersion
    {0x14, cl::kNoFkcl, true, true, true},    // GetDupAvailability
    {0x15, cl::kNoFkcl, true, false, false},  // (past the last)
    {0x16, cl::kNoFkcl, true, false, false},
};

void TestMap() {
    std::printf("C1: the map of the twenty ids of dlp:CLNT (and 0, 0x15, 0x16)\n");
    CHECK(sizeof(kWant) / sizeof(kWant[0]) == 0x17);
    u32 used = 0, posted = 0, macs = 0;
    for (const Want& w : kWant) {
        CHECK(cl::ToFkcl(w.id) == w.fkcl);
        CHECK(cl::IsInline(w.id) == w.inline_);
        CHECK(cl::UsedByDlplay(w.id) == w.used);
        CHECK(cl::CarriesMac(w.id) == w.mac);
        used += w.used ? 1 : 0;
        posted += w.inline_ ? 0 : 1;
        macs += w.mac ? 1 : 0;
        if (cl::IsInline(w.id) != w.inline_ || cl::ToFkcl(w.id) != w.fkcl || cl::UsedByDlplay(w.id) != w.used || cl::CarriesMac(w.id) != w.mac) {
            std::printf("  id 0x%02X differs from the table\n", static_cast<unsigned>(w.id));
        }
    }
    CHECK(used == 15);
    CHECK(posted == 6);
    CHECK(macs == 7);
    // far ids: never a set's, never a twin, inline (an error answered on the caller's thread)
    for (u32 id : {0x20u, 0x31u, 0xFFu, 0x10000u, 0xFFFFFFFFu}) {
        CHECK(cl::ToFkcl(id) == cl::kNoFkcl && cl::IsInline(id) && !cl::UsedByDlplay(id) && !cl::CarriesMac(id));
    }
    CHECK(cl::kLast == 0x14 && cl::kStartSession == 0x0D && cl::kGetMyStatus == 0x0E && cl::kGetWirelessRebootPassphrase == 0x11);
}

void TestUsesRoom() {
    std::printf("C2: the room's Download Play only for a plugin that joins a room by itself (Sources/main.cpp g_want_connected)\n");
    for (int k = 0; k < 8; ++k) {
        const bool host = (k & 1) != 0, autoconnect = (k & 2) != 0, menu_off = (k & 4) != 0;
        const bool want_connected = (autoconnect || menu_off) && host; // (Sources/main.cpp, with !cfg.host.empty() as host)
        CHECK(cl::UsesRoom(host, autoconnect, menu_off) == want_connected);
    }
    CHECK(!cl::UsesRoom(false, true, true));
    CHECK(!cl::UsesRoom(true, false, false));
    CHECK(cl::UsesRoom(true, true, false) && cl::UsesRoom(true, false, true));
}

void TestShownWord() {
    std::printf("C4: GetMyStatus shows the distribution's state 6 as 7, every other word as it is\n");
    const u32 in_session = (1u << 16) | 3u; // (in the session, node 3)
    for (u32 st : {0u, 1u, 2u, 5u, 9u, 0x80u}) {
        CHECK(cl::ShownWord(st << 24) == st << 24);
        CHECK(cl::ShownWord((st << 24) | in_session) == ((st << 24) | in_session));
    }
    CHECK(cl::ShownWord((6u << 24) | in_session) == ((7u << 24) | in_session));
    CHECK(cl::ShownWord(6u << 24) == 7u << 24);
    CHECK(cl::ShownWord((6u << 24) | 0x00FFFFFFu) == ((7u << 24) | 0x00FFFFFFu));
    CHECK(cl::ShownWord(7u << 24) == 7u << 24);
    CHECK(cl::ShownWord(0x06u << 16) == (0x06u << 16)); // (a 6 that is not the state is not touched)
}


// ---- the book of dlp:CLNT (azl/dlp_clnt_book.hpp) -------------------------------------------------------------------------------------------

azl::Group G(azl::u8 last, azl::u32 child_low, const char* name, azl::u8 total = 1, azl::u8 max = 4) {
    azl::Group g;
    std::memset(&g, 0, sizeof(g));
    g.host = {0x02, 0x10, 0x20, 0x30, 0x40, last};
    g.flags = 1;
    g.max_nodes = max;
    g.child_tid_low = child_low;
    g.total_nodes = total;
    g.signal = 3;
    for (u32 i = 0; name[i] != '\0' && i < azl::kNameUnits; ++i) {
        g.name[i] = static_cast<azl::u16>(name[i]);
    }
    return g;
}

/// Frame 0 of an announcement: the sizes at +0x38 / +0x3C and the short name at +0x40, big-endian (as Azahar's host and ours write it).
void Frame0(azl::u8 (&f)[0x300], azl::u32 transfer, azl::u32 required, const char* name) {
    std::memset(f, 0, sizeof(f));
    azl::StoreBE32(f + 0x38, transfer);
    azl::StoreBE32(f + 0x3C, required);
    for (u32 i = 0; name[i] != '\0'; ++i) {
        f[0x40 + 2 * i] = 0;
        f[0x40 + 2 * i + 1] = static_cast<azl::u8>(name[i]);
    }
}

void TestBook() {
    std::printf("C5: the book: records delivered once per scan, never one evicted, titles from frame 0, the children's table, the seam\n");
    azl::DlpClntBook b;
    CHECK(b.records() == 0 && b.Find(G(1, 0x0010C000, "A").host) == nullptr);
    // delivered, found, marked; a new scan delivers again; refresh changes a known record only
    const azl::Group a = G(1, 0x0010C000, "A"), c = G(2, 0x000F8200, "C");
    CHECK(!b.DeliveredThisScan(a.host));
    CHECK(b.Deliver(a));
    CHECK(b.DeliveredThisScan(a.host) && !b.DeliveredThisScan(c.host));
    CHECK(b.Find(a.host) != nullptr && b.Find(a.host)->child_tid_low == 0x0010C000u);
    b.NewScan();
    CHECK(!b.DeliveredThisScan(a.host));
    CHECK(b.Find(a.host) != nullptr);  // NewScan never forgets a record
    CHECK(b.Deliver(a) && b.records() == 1);  // (the same host: its record, not a second one)
    azl::Group a2 = a;
    a2.total_nodes = 3;
    b.Refresh(a2);
    CHECK(b.Find(a.host)->total_nodes == 3);
    b.Refresh(c);  // (unknown: nothing)
    CHECK(b.Find(c.host) == nullptr && b.records() == 1);
    // sixteen records, the seventeenth refused, the first sixteen still there (nothing evicted, even in a new scan)
    for (azl::u8 i = 2; i <= 16; ++i) {
        CHECK(b.Deliver(G(i, 0x00010000u + i, "X")));
    }
    CHECK(b.records() == 16);
    CHECK(!b.Deliver(G(17, 0x00011100, "Y")));
    b.NewScan();
    CHECK(!b.Deliver(G(18, 0x00011200, "Z")));
    for (azl::u8 i = 1; i <= 16; ++i) {
        CHECK(b.Find(G(i, 0, "").host) != nullptr);
    }
    CHECK(b.Deliver(G(16, 0x00010010, "X")));  // (a known one is delivered again in the new scan)
    // the titles of frame 0: sizes and the short name; a frame shorter than 0xC0 gives nothing; four slots, the oldest replaced
    azl::u8 f[0x300];
    Frame0(f, 0x1000, 0x2000, "Mario Party: Island Tour");
    b.NoteTitle(a.host, 0x000F8200, f, 0xBF);
    CHECK(b.TitleOf(a.host) == nullptr);
    b.NoteTitle(a.host, 0x000F8200, nullptr, 0x300);
    CHECK(b.TitleOf(a.host) == nullptr);
    b.NoteTitle(a.host, 0x000F8200, f, 0xC0);
    CHECK(b.TitleOf(a.host) != nullptr && b.TitleOf(a.host)->transfer == 0x1000u && b.TitleOf(a.host)->required == 0x2000u);
    CHECK(b.TitleOf(a.host)->short_be[1] == 'M' && b.TitleOf(a.host)->short_be[0] == 0);
    Frame0(f, 0x1111, 0x2222, "B");
    b.NoteTitle(a.host, 0x000F8200, f, 0x300);  // (the same host: its slot, refreshed)
    CHECK(b.TitleOf(a.host)->transfer == 0x1111u);
    for (azl::u8 i = 2; i <= 4; ++i) {
        b.NoteTitle(G(i, 0, "").host, 0x000F8200, f, 0x300);
    }
    CHECK(b.TitleOf(a.host) != nullptr);  // four slots: all four kept
    b.NoteTitle(G(5, 0, "").host, 0x000F8200, f, 0x300);
    CHECK(b.TitleOf(a.host) == nullptr);  // the fifth replaces the oldest (a's)
    CHECK(b.TitleOf(G(2, 0, "").host) != nullptr && b.TitleOf(G(5, 0, "").host) != nullptr);
    // Reset: records, titles and marks go; the children's source stays
    std::atomic<const azl::dlp_children::Table*> source{nullptr};
    b.SetChildrenSource(&source);
    b.Reset();
    CHECK(b.records() == 0 && b.TitleOf(G(2, 0, "").host) == nullptr && b.Find(a.host) == nullptr);
    azl::dlp_children::Table table;
    azl::dlp_children::Clear(&table);
    source.store(&table);
    CHECK(b.children() == &table);
    // the verdicts through the table (contract K3): no table -> Unknown; no row -> NoRow; a ready row with its block not read -> Unknown and the
    // block asked for; read -> Ready unless the sizes heard differ; missing -> NotPrepared
    source.store(nullptr);
    CHECK(b.VerdictOf(a) == azl::dlp_children::Verdict::Unknown);
    CHECK(b.TakeWantedBlocks() == 0);
    source.store(&table);
    CHECK(b.VerdictOf(a) == azl::dlp_children::Verdict::NoRow);
    azl::dlp_children::Row& row = table.index.rows[3];
    table.index.count = 4;
    row.child = 0x000400010010C000ull;
    row.game = 0x000400000010C000ull;
    row.prepared = 0x000400000F90C000ull;
    row.where = 'S';
    table.verdict[3] = static_cast<azl::u8>(azl::dlp_children::Verdict::Ready);
    CHECK(b.VerdictOf(a) == azl::dlp_children::Verdict::Unknown);
    CHECK(b.TakeWantedBlocks() == (1u << 3));
    CHECK(b.TakeWantedBlocks() == 0);  // (taken)
    CHECK(b.PreparedFor(a) == 0x000400000F90C000ull);
    CHECK(b.PreparedFor(c) == 0);
    CHECK(b.BlockFor(a) == nullptr);
    static azl::u8 block[azl::dlp_children::kBlockBytes];
    std::memset(block, 0, sizeof(block));
    azl::StoreLE32(block + azl::dlp_children::kBlockTransferAt, 0x5000);
    azl::StoreLE32(block + azl::dlp_children::kBlockRequiredAt, 0x6000);
    CHECK(azl::dlp_children::PutBlock(&table, 3, block));
    CHECK(b.BlockFor(a) == block);
    CHECK(b.VerdictOf(a) == azl::dlp_children::Verdict::Ready);  // (no title heard: sizes 0, Ready)
    Frame0(f, 0x5000, 0x6000, "T");
    b.NoteTitle(a.host, 0x0010C000, f, 0x300);
    CHECK(b.VerdictOf(a) == azl::dlp_children::Verdict::Ready);
    CHECK(b.TakeWantedBlocks() == 0);  // (a read block is never asked for again)
    Frame0(f, 0x5001, 0x6000, "T");
    b.NoteTitle(a.host, 0x0010C000, f, 0x300);
    CHECK(b.VerdictOf(a) == azl::dlp_children::Verdict::OtherVersion);
    // (the final review of 1.1.4) a host the book has a record of keeps the sizes it announced when four later hosts take every title slot: another
    // version stays another version (with the title's sizes alone it was "not heard", Ready, "join it from the 3DS Link app")
    CHECK(b.Deliver(a));  // (its title was heard before: the new record takes its sizes)
    for (azl::u8 i = 2; i <= 5; ++i) {
        b.NoteTitle(G(i, 0, "").host, 0x0010C000, f, 0x300);
    }
    CHECK(b.TitleOf(a.host) == nullptr);
    CHECK(b.VerdictOf(a) == azl::dlp_children::Verdict::OtherVersion);
    Frame0(f, 0x5000, 0x6000, "T");
    b.NoteTitle(a.host, 0x0010C000, f, 0x300);  // (heard again, the same version now: the record follows it)
    for (azl::u8 i = 2; i <= 5; ++i) {
        b.NoteTitle(G(i, 0, "").host, 0x0010C000, f, 0x300);
    }
    CHECK(b.TitleOf(a.host) == nullptr);
    CHECK(b.VerdictOf(a) == azl::dlp_children::Verdict::Ready);
    CHECK(b.VerdictOf(G(9, 0x0010C000, "D")) == azl::dlp_children::Verdict::Ready);  // (no record, no title: nothing heard, as ever)
    Frame0(f, 0x5001, 0x6000, "T");
    // NoteTitle asks for the block of the child a heard host announces (only that one: the network thread, at frame 0)
    azl::dlp_children::Table t2;
    azl::dlp_children::Clear(&t2);
    t2.index.count = 2;
    t2.index.rows[1] = row;
    t2.verdict[1] = static_cast<azl::u8>(azl::dlp_children::Verdict::Ready);
    source.store(&t2);
    b.NoteTitle(c.host, 0x000F8200, f, 0x300);  // (no row for Island Tour's child: nothing asked)
    CHECK(b.TakeWantedBlocks() == 0);
    b.NoteTitle(c.host, 0x0010C000, f, 0x300);
    CHECK(b.TakeWantedBlocks() == (1u << 1));
    b.WantRows(1u << 5);
    CHECK(b.TakeWantedBlocks() == (1u << 5));
    azl::dlp_children::NoBlock(&t2, 1);
    CHECK(b.VerdictOf(a) == azl::dlp_children::Verdict::NotPrepared);
    // the seam to G3 (K7): armed with the row's prepared title, the passphrase only while armed, everything off at Disarm / ForceLost / Reset
    CHECK(b.armed_prepared() == 0 && !b.passphrase_ready());
    b.NotePassphrase();
    CHECK(!b.passphrase_ready());
    b.Arm(0x000400000F90C000ull);
    CHECK(b.armed_prepared() == 0x000400000F90C000ull);
    b.NotePassphrase();
    CHECK(b.passphrase_ready());
    b.ForceLost();
    CHECK(b.armed_prepared() == 0 && !b.passphrase_ready());
    b.Arm(0x000400000F90C000ull);
    b.Reset();
    CHECK(b.armed_prepared() == 0);
    b.Arm(0);
    CHECK(b.armed_prepared() == 0);
    // the refusals the plugin's thread notices
    CHECK(b.refusals() == 0);
    b.NoteRefusal(azl::dlp_children::Verdict::OtherVersion);
    b.NoteRefusal(azl::dlp_children::Verdict::NoRow);
    CHECK(b.refusals() == 2 && b.refused_verdict() == azl::dlp_children::Verdict::NoRow);
    // GetMyStatus: forced 0x80, cleared by ClearOverride and NewScan
    CHECK(b.Shown(0x05010002u) == 0x05010002u);
    CHECK(b.Shown(0x06010002u) == 0x07010002u);
    b.ForceLost();
    CHECK(b.Shown(0x05010002u) == 0x80000000u);
    b.ClearOverride();
    CHECK(b.Shown(0x05010002u) == 0x05010002u);
    b.ForceLost();
    b.NewScan();
    CHECK(b.Shown(0x02000000u) == 0x02000000u);
}

// The model of 1-G1.md section 1.2: the target after a command, from the target before (None, Idle, Scan, Stop, StopSession, Final).
u8 DeferModel(u32 id, u8 d) {
    using B = azl::DlpClntBook;
    switch (id) {
    case 0x01: return B::kDeferIdle;
    case 0x05: return B::kDeferScan;
    case 0x02: return B::kDeferFinal;
    case 0x06: {
        static const u8 t[6] = {B::kDeferStop, B::kDeferIdle, B::kDeferIdle, B::kDeferStop, B::kDeferStopSession, B::kDeferFinal};
        return t[d];
    }
    case 0x12: {
        static const u8 t[6] = {B::kDeferStopSession, B::kDeferIdle, B::kDeferScan, B::kDeferStopSession, B::kDeferStopSession, B::kDeferFinal};
        return t[d];
    }
    }
    return 0xFF;
}

void TestDefer() {
    std::printf("C6: the work deferred with no network thread: every sequence of up to three posted commands against the table of the spec\n");
    using B = azl::DlpClntBook;
    const u32 ids[5] = {0x01, 0x05, 0x06, 0x12, 0x02};
    u32 sequences = 0;
    for (u32 n = 1; n <= 3; ++n) {
        u32 total = 1;
        for (u32 k = 0; k < n; ++k) {
            total *= 5;
        }
        for (u32 code = 0; code < total; ++code) {
            B b;
            u8 model = B::kDeferNone;
            u32 c = code;
            for (u32 k = 0; k < n; ++k) {
                const u32 id = ids[c % 5];
                c /= 5;
                u32 res = 0xDEAD;
                CHECK(b.Defer(id, 3, &res));
                CHECK(res == 0);
                model = DeferModel(id, model);
                CHECK(b.deferred() == model);
                const u32 shown = b.Shown(0x05010002u) >> 24;
                CHECK(shown == (model == B::kDeferScan ? 2u : 1u));
            }
            u8 beacons = 0;
            CHECK(b.TakeDeferred(&beacons) == model);
            CHECK(b.deferred() == B::kDeferNone);
            CHECK(b.Shown(0x05010002u) == 0x05010002u);
            ++sequences;
        }
    }
    CHECK(sequences == 5 + 25 + 125);
    // Initialize's beacons are handed over; StartSession and every other id cannot wait
    B b;
    u32 res = 0;
    CHECK(b.Defer(0x01, 16, &res) && res == 0);
    u8 beacons = 0;
    CHECK(b.TakeDeferred(&beacons) == B::kDeferIdle && beacons == 16);
    for (u32 id = 0; id <= 0x16; ++id) {
        if (id == 0x01 || id == 0x02 || id == 0x05 || id == 0x06 || id == 0x12) {
            continue;
        }
        res = 0;
        CHECK(!b.Defer(id, 4, &res));
        CHECK(res == 0xC940AC04u);
        CHECK(b.deferred() == B::kDeferNone);
    }
    // StopSession lifts the 0x80 forced; Initialize and Finalize empty the book
    b.ForceLost();
    CHECK(b.Defer(0x12, 0, &res) && b.Shown(0) >> 24 == 1u);
    CHECK(b.Deliver(G(1, 1, "a")));
    CHECK(b.Defer(0x02, 0, &res) && b.records() == 0);
}

void TestWriters() {
    std::printf("C7: the records of dlp:CLNT byte for byte, in buffers filled with 0xCC between sentinels\n");
    static azl::u8 buf[16 + 0x13A8 + 16];
    std::memset(buf, 0xCC, sizeof(buf));
    std::memset(buf, 0x5A, 16);
    std::memset(buf + 16 + 0x13A8, 0x5A, 16);
    azl::u8* ti = buf + 16;
    azl::DlpClntBook b;
    const azl::Group g = G(7, 0x000F8205, "Gionko", 2, 4);
    azl::u8 f[0x300];
    Frame0(f, 0x1000, 0x2000, "Mario Party: Island Tour");
    b.NoteTitle(g.host, g.child_tid_low, f, 0x300);
    azl::WriteClntTitleInfo(ti, g, b.TitleOf(g.host), nullptr, azl::dlp_children::Verdict::NoRow);
    CHECK(azl::LoadLE32(ti + 0x00) == 0x000F82u);
    CHECK(azl::LoadLE32(ti + 0x04) == 0x05u);
    CHECK(std::memcmp(ti + 0x08, g.host.data(), 6) == 0);
    CHECK(ti[0x0E] == 0 && ti[0x0F] == 0);
    const char* name = "Mario Party: Island Tour";
    for (u32 i = 0; i < 64; ++i) {
        CHECK(azl::LoadLE16(ti + 0x20 + 2 * i) == (i < std::strlen(name) ? static_cast<azl::u16>(name[i]) : 0u));  // (big-endian turned)
    }
    const char* tail = "Hosted by Gionko - turn this game on in 3DS Link";
    for (u32 i = 0; i < 128; ++i) {
        CHECK(azl::LoadLE16(ti + 0xA0 + 2 * i) == (i < std::strlen(tail) ? static_cast<azl::u16>(tail[i]) : 0u));
    }
    bool zero = true;
    for (u32 i = 0x10; i < 0x20; ++i) {
        zero = zero && ti[i] == 0;  // the age ratings: 0
    }
    for (u32 i = 0x1A0; i < 0x13A8; ++i) {
        zero = zero && ti[i] == 0;  // no block: the icon and the size stay 0
    }
    CHECK(zero);
    for (u32 i = 0; i < 16; ++i) {
        CHECK(buf[i] == 0x5A && buf[16 + 0x13A8 + i] == 0x5A);
    }
    // with a block: its name, icon and size over the frame's; title_long as the verdict says; with no title heard: the child's eight digits
    static azl::u8 block[azl::dlp_children::kBlockBytes];
    for (u32 i = 0; i < sizeof(block); ++i) {
        block[i] = static_cast<azl::u8>(i * 7 + 1);
    }
    azl::StoreLE32(block + azl::dlp_children::kBlockTransferAt, 0x00ABCDEF);
    azl::WriteClntTitleInfo(ti, g, nullptr, block, azl::dlp_children::Verdict::Ready);
    CHECK(std::memcmp(ti + 0x20, block, 128) == 0);
    CHECK(std::memcmp(ti + 0x1A0, block + 384, 0x1200) == 0);
    CHECK(azl::LoadLE32(ti + 0x13A0) == 0x00ABCDEFu);
    CHECK(azl::LoadLE16(ti + 0xA0) == 'H');
    azl::WriteClntTitleInfo(ti, g, nullptr, nullptr, azl::dlp_children::Verdict::Unknown);
    const char* digits = "000F8205";
    for (u32 i = 0; i < 8; ++i) {
        CHECK(azl::LoadLE16(ti + 0x20 + 2 * i) == static_cast<azl::u16>(digits[i]));
    }
    CHECK(azl::LoadLE16(ti + 0x20 + 16) == 0);
    // DLPServerInfo: MAC, signal, max, min(total, max), the small line, the tick; the other node_info and the rest zero
    static azl::u8 sbuf[16 + 0x298 + 16];
    std::memset(sbuf, 0xCC, sizeof(sbuf));
    std::memset(sbuf, 0x5A, 16);
    std::memset(sbuf + 16 + 0x298, 0x5A, 16);
    azl::u8* si = sbuf + 16;
    azl::Group over = g;
    over.total_nodes = 9;  // (more than the ceiling: +9 is the ceiling)
    azl::WriteClntServerInfo(si, over, azl::dlp_children::Verdict::Ready, 0x1122334455667788ull);
    CHECK(std::memcmp(si, g.host.data(), 6) == 0);
    CHECK(si[6] == 0 && si[7] == 3 && si[8] == 4 && si[9] == 4);
    const char* line = "Gionko OK";
    for (u32 i = 0; i < 10; ++i) {
        CHECK(azl::LoadLE16(si + 0x1C + 2 * i) == (i < std::strlen(line) ? static_cast<azl::u16>(line[i]) : 0u));
    }
    CHECK(azl::LoadLE64(si + 0x290) == 0x1122334455667788ull);
    zero = true;
    for (u32 i = 0x0A; i < 0x1C; ++i) {
        zero = zero && si[i] == 0;
    }
    for (u32 i = 0x30; i < 0x290; ++i) {
        zero = zero && si[i] == 0;  // (+0x30/+0x32 too: no "???", and the other 15 nodes)
    }
    CHECK(zero);
    for (u32 i = 0; i < 16; ++i) {
        CHECK(sbuf[i] == 0x5A && sbuf[16 + 0x298 + i] == 0x5A);
    }
    azl::WriteClntServerInfo(si, g, azl::dlp_children::Verdict::Unknown, 1);
    CHECK(si[9] == 2 && azl::LoadLE16(si + 0x1C) == 'G' && azl::LoadLE16(si + 0x1C + 12) == 0);  // ("Gionko" as it is)
}

} // namespace

int main() {
    TestMap();
    TestUsesRoom();
    TestShownWord();
    TestBook();
    TestDefer();
    TestWriters();
    if (g_failures != 0) {
        std::printf("dlp_clnt_test: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("dlp_clnt_test: all %d checks passed\n", g_checks);
    return 0;
}
