// The wire of the real Download Play (azl/dlp_real_wire.hpp, 1.1.3) against the golden bytes of the capture 1c (Tests/dlp_real_luigi_golden.inc,
// two Azahar emulators playing Luigi's Mansion 2018): tests W1..W10 of PROGETTO-DLP-VERO-1.1.3.md, section 7.2.
//   W1   the checksum (seed ff a4 72 ff) of every golden packet is the one captured at +8..11; the group of t=739.896 has the same ones
//   W1b  the same with the seed 00 00 00 00: the values of the design's appendix A (what we send)
//   W2   frame 0 with the child's title id, max 2 and +0x38..+0x2F7 taken from the golden (its reduced zone): the 768 golden bytes
//   W3   frames 1..3 with the golden's icon zone, and frame 4 (all zero): the golden bytes
//   W4   our frame 0 (option c): the golden's bytes in +0x00..0x07, +0x0C..0x37, +0x2F8..0x2FF, zero in +0x38..0x2F7
//   W5   the library's dlph_pk_auth / startdist / finish_req / begin_final and the codec's Auth / StartDist / Finish / Final: A1, A3, A5, A7
//   W6   the codec's notice: A8 (and not the library's begin_ping)
//   W7   the fake client's answers: A2 from A1, A4 from A3, A6 from A5, A9 from A8, with their lengths
//   W8   the parsers take every golden packet with its kind and index
//   W9   the parsers refuse, with the reason, what is short, has another size, an index >= 5, an unknown type, a notice of 0x18 with index 1;
//        an A4 that wants the content is read as such; a notice with sequence 4 or 5 is taken; nothing is read past the length (ASan:
//        every packet is copied into a heap block of exactly its length, and every prefix of every golden is parsed)
//   W10  the checksum with 1, 2 and 3 trailing bytes against a model written here; one bit changed changes it more often than not
//        (NOT always: the step loses bits, see TestChecksumEdges); the step is (h >> s) | (h << s), not a rotation; +8..11 does not count
// Build (Tests/run.sh):
//   gcc -O1 -g -fsanitize=address,undefined -c ConfigApp/dlph/dlph_wire.c -o dlph_wire.o
//   g++ -std=c++17 -O1 -g -Wall -Wextra -IIncludes -fsanitize=address,undefined Tests/dlp_real_wire_test.cpp Sources/azl/dlp_real_wire.cpp dlph_wire.o
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "azl/dlp_real_wire.hpp"
#include "../ConfigApp/dlph/dlph_wire.h"
#include "dlp_real_luigi_golden.inc"

extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

using namespace azl;
using namespace azl::dlp_real;

static int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                              \
    } while (0)
#define CHECK_EQ(a, b)                                                                                                 \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        const unsigned long long va_ = static_cast<unsigned long long>(a), vb_ = static_cast<unsigned long long>(b);   \
        if (va_ != vb_) {                                                                                              \
            ++g_failures;                                                                                              \
            std::fprintf(stderr, "  FAIL %s:%d: %s == %s (0x%llx vs 0x%llx)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
        }                                                                                                              \
    } while (0)

static const u8 kZeroSeed[4] = {0, 0, 0, 0};
static const u8 kRid[2] = {0x01, 0x02};
static const u64 kLuigiChild = 0x00040001001D1A00ULL;

/// A heap block of exactly `len` bytes holding a copy of `src` (AddressSanitizer reports a read one byte past it).
static std::unique_ptr<u8[]> Exact(const u8* src, std::size_t len) {
    std::unique_ptr<u8[]> b(new u8[len ? len : 1]);
    if (len) std::memcpy(b.get(), src, len);
    return b;
}

static bool Same(const u8* a, const u8* b, std::size_t len) {
    return std::memcmp(a, b, len) == 0;
}

static bool AllZero(const u8* p, std::size_t from, std::size_t to) {
    for (std::size_t i = from; i < to; ++i)
        if (p[i]) return false;
    return true;
}

static const DlpRealGolden& Dialog(const char* id) {
    for (const DlpRealGolden& g : kDlpRealLuigiDialog)
        if (std::strcmp(g.id, id) == 0) return g;
    std::fprintf(stderr, "no golden %s\n", id);
    std::abort();
}

// ---- W1, W1b ----
static void TestChecksums() {
    for (const DlpRealGolden& g : kDlpRealLuigiAnnounce) {
        CHECK_EQ(Checksum(kDlpRealLuigiSeed, g.bytes, g.size), g.checksum_capture);
        CHECK_EQ(LoadBE32(g.bytes + 8), g.checksum_capture);
        CHECK_EQ(dlph_checksum(kDlpRealLuigiSeed, g.bytes, g.size), g.checksum_capture);
        CHECK_EQ(Checksum(kZeroSeed, g.bytes, g.size), g.checksum_zero);
        CHECK_EQ(Checksum(kTxSeed, g.bytes, g.size), g.checksum_zero);
    }
    for (unsigned i = 0; i < kAnnounceFrames; ++i)
        CHECK_EQ(kDlpRealLuigiAnnounceChecksumsAt739[i], kDlpRealLuigiAnnounce[i].checksum_capture);
    for (const DlpRealGolden& g : kDlpRealLuigiDialog) {
        CHECK_EQ(Checksum(kDlpRealLuigiSeed, g.bytes, g.size), g.checksum_capture);
        CHECK_EQ(LoadBE32(g.bytes + 8), g.checksum_capture);
        CHECK(dlph_check(kDlpRealLuigiSeed, g.bytes, g.size));
        CHECK_EQ(Checksum(kZeroSeed, g.bytes, g.size), g.checksum_zero);
    }
    // the design's appendix A, seed 0
    static const u32 kAnnounceZero[5] = {0xbdfff7f2, 0xafbfffd7, 0xbebfffef, 0x1195bfbf, 0xbfbfbf02};
    for (unsigned i = 0; i < kAnnounceFrames; ++i) CHECK_EQ(kDlpRealLuigiAnnounce[i].checksum_zero, kAnnounceZero[i]);
    CHECK_EQ(Checksum(kZeroSeed, Dialog("A1").bytes, kAuthBytes), 0x17171602);
    CHECK_EQ(Checksum(kZeroSeed, Dialog("A2").bytes, kAuthAckBytes), 0x1b1b1b03);
    CHECK_EQ(Checksum(kZeroSeed, Dialog("A3").bytes, kStartDistBytes), 0x17171703);
    CHECK_EQ(Checksum(kZeroSeed, Dialog("A4").bytes, kDistAckBytes), 0x1f1f1b03);
    CHECK_EQ(Checksum(kZeroSeed, Dialog("A5").bytes, kFinishBytes), 0x1f1f1f03);
    CHECK_EQ(Checksum(kZeroSeed, Dialog("A6").bytes, kFinishAckBytes), 0x27272303);
    CHECK_EQ(Checksum(kZeroSeed, Dialog("A7").bytes, kFinalBytes), 0xb7b7bfb9);
    CHECK_EQ(Checksum(kZeroSeed, Dialog("A8").bytes, kNoticeBytes), 0x1d1f1f03);
    CHECK_EQ(Checksum(kZeroSeed, Dialog("A9").bytes, kNoticeAckBytes), 0x1d1f1f03);
    // the group is 6576 bytes
    std::size_t group = 0;
    for (const DlpRealGolden& g : kDlpRealLuigiAnnounce) group += g.size;
    CHECK_EQ(group, kAnnounceGroupBytes);
}

// ---- W2, W3, W4 ----
static void TestAnnounce() {
    u8 buf[kBufferBytes];
    // W2: frame 0 with the golden's +0x38..+0x2F7 injected (the sizes and the reduced zone of titles and icon)
    const DlpRealGolden& f0 = kDlpRealLuigiAnnounce[0];
    std::memset(buf, 0xA5, sizeof buf);
    CHECK_EQ(AnnounceFrame(buf, 0, kLuigiChild, 2, kDlpRealLuigiSeed), kFrame0Bytes);
    CHECK_EQ(LoadBE32(buf + 8), Checksum(kDlpRealLuigiSeed, buf, kFrame0Bytes));
    std::memcpy(buf + 0x38, f0.bytes + 0x38, 0x2F8 - 0x38);
    Seal(kDlpRealLuigiSeed, buf, kFrame0Bytes);
    CHECK(Same(buf, f0.bytes, kFrame0Bytes));
    CHECK_EQ(LoadBE32(buf + 8), 0xa85b0d4c);
    // W3: frames 1..3 with the golden's icon zone, frame 4 as it is
    for (unsigned i = 1; i < 4; ++i) {
        const DlpRealGolden& g = kDlpRealLuigiAnnounce[i];
        std::memset(buf, 0x5A, sizeof buf);
        CHECK_EQ(AnnounceFrame(buf, i, kLuigiChild, 2, kDlpRealLuigiSeed), kIconFrameBytes);
        CHECK(AllZero(buf, kHeaderBytes, kIconFrameBytes));  // ours carries no icon
        std::memcpy(buf + kHeaderBytes, g.bytes + kHeaderBytes, kIconFrameBytes - kHeaderBytes);
        Seal(kDlpRealLuigiSeed, buf, kIconFrameBytes);
        CHECK(Same(buf, g.bytes, kIconFrameBytes));
    }
    std::memset(buf, 0x5A, sizeof buf);
    CHECK_EQ(AnnounceFrame(buf, 4, kLuigiChild, 2, kDlpRealLuigiSeed), kFrame4Bytes);
    CHECK(Same(buf, kDlpRealLuigiAnnounce[4].bytes, kFrame4Bytes));
    CHECK_EQ(LoadBE32(buf + 8), 0xe8470ccf);
    // the frame 4 the codec writes needs no golden zone: its checksum with seed 0 is the appendix's
    CHECK_EQ(AnnounceFrame(buf, 4, kLuigiChild, 2, kTxSeed), kFrame4Bytes);
    CHECK_EQ(LoadBE32(buf + 8), 0xbfbfbf02);
    // index 5 and over: nothing written
    std::memset(buf, 0x77, sizeof buf);
    CHECK_EQ(AnnounceFrame(buf, 5, kLuigiChild, 2, kTxSeed), 0u);
    CHECK_EQ(AnnounceFrame(buf, 200, kLuigiChild, 2, kTxSeed), 0u);
    CHECK_EQ(buf[0], 0x77);
    // W4: our frame 0 (option c), seed of the capture: the golden's bytes outside the checksum and the zone, zero inside it
    std::memset(buf, 0xA5, sizeof buf);
    CHECK_EQ(AnnounceFrame(buf, 0, kLuigiChild, 2, kDlpRealLuigiSeed), kFrame0Bytes);
    CHECK(Same(buf, f0.bytes, 8));
    CHECK(Same(buf + 0x0C, f0.bytes + 0x0C, 0x38 - 0x0C));
    CHECK(Same(buf + 0x2F8, f0.bytes + 0x2F8, kFrame0Bytes - 0x2F8));
    CHECK(AllZero(buf, 0x38, 0x2F8));
    CHECK(buf[kFrame0Bytes] == 0xA5);  // nothing written past the frame
    CHECK_EQ(AnnounceChildTid(buf), kLuigiChild);
    CHECK_EQ(AnnounceMaxNodes(buf), 2);
    // the other max_nodes and title ids land where they should
    CHECK_EQ(AnnounceFrame(buf, 0, 0x0004000100183600ULL, 4, kTxSeed), kFrame0Bytes);
    CHECK_EQ(AnnounceChildTid(buf), 0x0004000100183600ULL);
    CHECK_EQ(AnnounceMaxNodes(buf), 4);
    CHECK_EQ(kChildTidHigh | 0x001D1A00u, kLuigiChild);
    // every frame of ours is taken by the parser with its index
    for (unsigned i = 0; i < kAnnounceFrames; ++i) {
        const std::size_t n = AnnounceFrame(buf, i, kLuigiChild, 2, kTxSeed);
        auto b = Exact(buf, n);
        const Packet r = ParseAnnounce(b.get(), n);
        CHECK(r.kind == Kind::Announce && r.why == Why::Ok);
        CHECK_EQ(r.index, i);
        CHECK_EQ(LoadBE32(buf + 8), Checksum(kTxSeed, buf, n));
    }
}

// ---- W5, W6 ----
static void TestHostPackets() {
    u8 buf[kBufferBytes], lib[DLPH_MAX_FRAME];
    const u8 pass[kPassphraseBytes] = {'5', '0', 'e', '4', 'd', '8', '8', 'c', 0};
    // the library's builders
    CHECK_EQ(dlph_pk_auth(lib, kDlpRealLuigiSeed), kAuthBytes);
    CHECK(Same(lib, Dialog("A1").bytes, kAuthBytes));
    CHECK_EQ(dlph_pk_startdist(lib, kDlpRealLuigiSeed, 1, kRid), kStartDistBytes);
    CHECK(Same(lib, Dialog("A3").bytes, kStartDistBytes));
    CHECK_EQ(dlph_pk_finish_req(lib, kDlpRealLuigiSeed, 2, kRid, 0), kFinishBytes);
    CHECK(Same(lib, Dialog("A5").bytes, kFinishBytes));
    CHECK_EQ(dlph_pk_begin_final(lib, kDlpRealLuigiSeed, 3, kRid, pass), kFinalBytes);
    CHECK(Same(lib, Dialog("A7").bytes, kFinalBytes));
    // the codec's (the same, through it)
    CHECK_EQ(Auth(buf, kDlpRealLuigiSeed), kAuthBytes);
    CHECK(Same(buf, Dialog("A1").bytes, kAuthBytes));
    CHECK_EQ(StartDist(buf, kDlpRealLuigiSeed, 1, kRid), kStartDistBytes);
    CHECK(Same(buf, Dialog("A3").bytes, kStartDistBytes));
    CHECK_EQ(Finish(buf, kDlpRealLuigiSeed, 2, kRid), kFinishBytes);
    CHECK(Same(buf, Dialog("A5").bytes, kFinishBytes));
    CHECK_EQ(Final(buf, kDlpRealLuigiSeed, 3, kRid, pass), kFinalBytes);
    CHECK(Same(buf, Dialog("A7").bytes, kFinalBytes));
    // W6: the notice in Azahar's form
    std::memset(buf, 0xEE, sizeof buf);
    CHECK_EQ(Notice(buf, kDlpRealLuigiSeed, 3, kRid), kNoticeBytes);
    CHECK(Same(buf, Dialog("A8").bytes, kNoticeBytes));
    CHECK_EQ(buf[kNoticeBytes], 0xEE);
    // ... and the library's begin_ping is NOT it (index 1, 0x20 bytes, +0x1D = 8: an Azahar client's final with an empty passphrase)
    CHECK_EQ(dlph_pk_begin_ping(lib, kDlpRealLuigiSeed, 3, kRid), 0x20u);
    CHECK(lib[12] == 1 && lib[0x1D] == 8);
    auto ping = Exact(lib, 0x20);
    CHECK(ParseFromHost(ping.get(), 0x20).kind == Kind::Final);  // which is why it is never used
    // what we send is sealed with seed 0 (design 2.5): A1 with kTxSeed is the appendix's 17171602
    CHECK_EQ(Auth(buf, kTxSeed), kAuthBytes);
    CHECK_EQ(LoadBE32(buf + 8), 0x17171602);
    CHECK_EQ(Notice(buf, kTxSeed, 3, kRid), kNoticeBytes);
    CHECK_EQ(LoadBE32(buf + 8), 0x1d1f1f03);
    CHECK_EQ(Final(buf, kTxSeed, 3, kRid, pass), kFinalBytes);
    CHECK_EQ(LoadBE32(buf + 8), 0xb7b7bfb9);
    // another sequence goes to +0x0D and nowhere else
    CHECK_EQ(Notice(buf, kDlpRealLuigiSeed, 7, kRid), kNoticeBytes);
    CHECK_EQ(buf[13], 7);
    CHECK(Same(buf + 0x10, Dialog("A8").bytes + 0x10, kNoticeBytes - 0x10));
}

// ---- W7 ----
static void TestReplies() {
    u8 buf[kBufferBytes];
    struct Pair { const char* from; const char* to; std::size_t len; };
    static const Pair kPairs[] = {{"A1", "A2", kAuthAckBytes}, {"A3", "A4", kDistAckBytes}, {"A5", "A6", kFinishAckBytes}, {"A8", "A9", kNoticeAckBytes}};
    for (const Pair& p : kPairs) {
        const DlpRealGolden& from = Dialog(p.from);
        const DlpRealGolden& to = Dialog(p.to);
        auto b = Exact(from.bytes, from.size);
        const Packet got = ParseFromHost(b.get(), from.size);
        CHECK(got.why == Why::Ok);
        std::memset(buf, 0xCC, sizeof buf);
        const std::size_t n = Reply(buf, kDlpRealLuigiSeed, got);
        CHECK_EQ(n, p.len);
        CHECK_EQ(n, to.size);
        CHECK(Same(buf, to.bytes, to.size));
        CHECK_EQ(buf[n], 0xCC);
        // with our seed: the appendix's checksum
        CHECK_EQ(Reply(buf, kTxSeed, got), p.len);
        CHECK_EQ(LoadBE32(buf + 8), to.checksum_zero);
    }
    // the final and a rejected packet are not answered
    auto fin = Exact(Dialog("A7").bytes, kFinalBytes);
    const Packet final = ParseFromHost(fin.get(), kFinalBytes);
    CHECK(final.kind == Kind::Final);
    std::memset(buf, 0xCC, sizeof buf);
    CHECK_EQ(Reply(buf, kTxSeed, final), 0u);
    Packet bad{};
    CHECK_EQ(Reply(buf, kTxSeed, bad), 0u);
    Packet ack{};
    ack.kind = Kind::AuthAck;
    CHECK_EQ(Reply(buf, kTxSeed, ack), 0u);
    CHECK_EQ(buf[0], 0xCC);
    // the answer copies whatever sequence the host sent (an Azahar host counts on after every A9)
    u8 notice[kBufferBytes];
    CHECK_EQ(Notice(notice, kTxSeed, 5, kRid), kNoticeBytes);
    const Packet n5 = ParseFromHost(notice, kNoticeBytes);
    CHECK(n5.kind == Kind::Notice);
    CHECK_EQ(Reply(buf, kTxSeed, n5), kNoticeAckBytes);
    CHECK(buf[13] == 5 && buf[14] == 1 && buf[15] == 2 && buf[12] == 0);
}

// ---- W8 ----
static void TestParseGolden() {
    for (unsigned i = 0; i < kAnnounceFrames; ++i) {
        const DlpRealGolden& g = kDlpRealLuigiAnnounce[i];
        auto b = Exact(g.bytes, g.size);
        const Packet r = ParseAnnounce(b.get(), g.size);
        CHECK(r.kind == Kind::Announce && r.why == Why::Ok);
        CHECK_EQ(r.index, i);
        // a frame of the announcement is no packet of the dialogue
        CHECK(ParseFromHost(b.get(), g.size).kind == Kind::None);
        CHECK(ParseFromHost(b.get(), g.size).why == Why::Type);
        CHECK(ParseFromClient(b.get(), g.size).kind == Kind::None);
    }
    CHECK_EQ(AnnounceChildTid(kDlpRealLuigiAnnounce[0].bytes), kLuigiChild);
    CHECK_EQ(AnnounceMaxNodes(kDlpRealLuigiAnnounce[0].bytes), 2);
    struct Want { const char* id; bool from_host; Kind kind; u8 index; };
    static const Want kWant[] = {
        {"A1", true, Kind::Auth, 1},       {"A2", false, Kind::AuthAck, 0}, {"A3", true, Kind::StartDist, 1},
        {"A4", false, Kind::DistAck, 0},   {"A5", true, Kind::Finish, 1},   {"A6", false, Kind::FinishAck, 0},
        {"A7", true, Kind::Final, 1},      {"A8", true, Kind::Notice, 0},   {"A9", false, Kind::NoticeAck, 0},
    };
    for (const DlpRealGolden& g : kDlpRealLuigiDialog) {
        const Want* w = nullptr;
        for (const Want& x : kWant)
            if (std::strcmp(x.id, g.id) == 0) w = &x;
        CHECK(w != nullptr);
        if (!w) continue;
        CHECK_EQ(g.src_node, w->from_host ? 1u : 2u);
        auto b = Exact(g.bytes, g.size);
        const Packet r = w->from_host ? ParseFromHost(b.get(), g.size) : ParseFromClient(b.get(), g.size);
        CHECK(r.kind == w->kind);
        CHECK(r.why == Why::Ok);
        CHECK_EQ(r.index, w->index);
        CHECK(!r.needs_content);
        // a channel-2 packet is no frame of the announcement
        CHECK(ParseAnnounce(b.get(), g.size).kind == Kind::None);
    }
    // the fields the host and the client keep
    auto a2 = Exact(Dialog("A2").bytes, kAuthAckBytes);
    const Packet r2 = ParseFromClient(a2.get(), kAuthAckBytes);
    CHECK(r2.rid[0] == 0x01 && r2.rid[1] == 0x02);  // the client's id from the body
    auto a3 = Exact(Dialog("A3").bytes, kStartDistBytes);
    const Packet r3 = ParseFromHost(a3.get(), kStartDistBytes);
    CHECK(r3.seq == 1 && r3.rid[0] == 0x01 && r3.rid[1] == 0x02);
    auto a7 = Exact(Dialog("A7").bytes, kFinalBytes);
    CHECK(ParseFromHost(a7.get(), kFinalBytes).seq == 3);
    CHECK(Same(a7.get() + kPassphraseAt, reinterpret_cast<const u8*>("50e4d88c"), kPassphraseBytes));  // (with its zero)
    // the client's packets are no host packets of the same type (other lengths): A2, A4, A6
    for (const char* id : {"A2", "A4", "A6"}) {
        const DlpRealGolden& g = Dialog(id);
        auto b = Exact(g.bytes, g.size);
        CHECK(ParseFromHost(b.get(), g.size).kind == Kind::None);
    }
    // A8 and A9 are the same bytes (the notice and its answer): each side reads them as its own
    CHECK(Same(Dialog("A8").bytes, Dialog("A9").bytes, kNoticeBytes));
}

// ---- W9 ----
static Packet FromHost(const u8* src, std::size_t len) {
    auto b = Exact(src, len);
    return ParseFromHost(b.get(), len);
}
static Packet FromClient(const u8* src, std::size_t len) {
    auto b = Exact(src, len);
    return ParseFromClient(b.get(), len);
}
static Packet Announce(const u8* src, std::size_t len) {
    auto b = Exact(src, len);
    return ParseAnnounce(b.get(), len);
}

static void TestParseRefusals() {
    u8 m[kBufferBytes];
    // short by one byte, the size left as it was: Size; with the size made coherent: Length
    {
        const DlpRealGolden& a1 = Dialog("A1");
        Packet r = FromHost(a1.bytes, a1.size - 1);
        CHECK(r.kind == Kind::None && r.why == Why::Size);
        std::memcpy(m, a1.bytes, a1.size);
        StoreBE16(m + 4, static_cast<u16>(a1.size - 1));
        r = FromHost(m, a1.size - 1);
        CHECK(r.kind == Kind::None && r.why == Why::Length);
        const DlpRealGolden& a6 = Dialog("A6");
        std::memcpy(m, a6.bytes, a6.size);
        StoreBE16(m + 4, static_cast<u16>(a6.size - 1));
        r = FromClient(m, a6.size - 1);
        CHECK(r.kind == Kind::None && r.why == Why::Length);
        std::memcpy(m, kDlpRealLuigiAnnounce[0].bytes, kFrame0Bytes);
        StoreBE16(m + 4, kFrame0Bytes - 1);
        r = Announce(m, kFrame0Bytes - 1);
        CHECK(r.kind == Kind::None && r.why == Why::Length);
    }
    // longer than its kind, size coherent: Length (we take only the capture's lengths)
    {
        std::memset(m, 0, sizeof m);
        std::memcpy(m, Dialog("A5").bytes, kFinishBytes);
        StoreBE16(m + 4, kFinishBytes + 4);
        CHECK(FromHost(m, kFinishBytes + 4).why == Why::Length);
    }
    // shorter than a header
    for (std::size_t n = 0; n < kHeaderBytes; ++n) {
        CHECK(FromHost(Dialog("A1").bytes, n).why == Why::Short);
        CHECK(FromClient(Dialog("A2").bytes, n).why == Why::Short);
        CHECK(Announce(kDlpRealLuigiAnnounce[0].bytes, n).why == Why::Short);
    }
    // the size at +4 says another length
    {
        std::memcpy(m, Dialog("A2").bytes, kAuthAckBytes);
        StoreBE16(m + 4, kAuthAckBytes + 1);
        CHECK(FromClient(m, kAuthAckBytes).why == Why::Size);
        StoreBE16(m + 4, 0);
        CHECK(FromClient(m, kAuthAckBytes).why == Why::Size);
    }
    // an index >= 5 in the announcement; another index in the announcement's length
    {
        std::memcpy(m, kDlpRealLuigiAnnounce[4].bytes, kFrame4Bytes);
        for (u8 idx : {5, 6, 0x80, 0xFF}) {
            m[12] = idx;
            const Packet r = Announce(m, kFrame4Bytes);
            CHECK(r.kind == Kind::None && r.why == Why::Index);
        }
        m[12] = 0;  // a frame 0 of 1464 bytes
        CHECK(Announce(m, kFrame4Bytes).why == Why::Length);
        m[12] = 2;  // an icon frame of 1464 bytes
        CHECK(Announce(m, kFrame4Bytes).why == Why::Length);
        std::memcpy(m, kDlpRealLuigiAnnounce[1].bytes, kIconFrameBytes);
        m[12] = 4;  // a frame 4 of 1448 bytes
        CHECK(Announce(m, kIconFrameBytes).why == Why::Length);
        m[12] = 1;
        m[0] = 2;   // another type on channel 1
        CHECK(Announce(m, kIconFrameBytes).why == Why::Type);
    }
    // unknown types on channel 2 (4 = a fragment of the child: never in this mode; 0, 1, 7, 0xFF)
    for (u8 t : {0, 1, 4, 7, 0x42, 0xFF}) {
        std::memcpy(m, Dialog("A5").bytes, kFinishBytes);
        m[0] = t;
        CHECK(FromHost(m, kFinishBytes).why == Why::Type);
        std::memcpy(m, Dialog("A4").bytes, kDistAckBytes);
        m[0] = t;
        CHECK(FromClient(m, kDistAckBytes).why == Why::Type);
    }
    // a type 06 of 0x18 bytes with index 1 (the final is 0x20): refused; a final of 0x20 with index 0: refused; index 2: refused
    {
        std::memcpy(m, Dialog("A8").bytes, kNoticeBytes);
        m[12] = 1;
        Packet r = FromHost(m, kNoticeBytes);
        CHECK(r.kind == Kind::None && r.why == Why::Length);
        std::memcpy(m, Dialog("A7").bytes, kFinalBytes);
        m[12] = 0;
        r = FromHost(m, kFinalBytes);
        CHECK(r.kind == Kind::None && r.why == Why::Length);
        m[12] = 2;
        r = FromHost(m, kFinalBytes);
        CHECK(r.kind == Kind::None && r.why == Why::Index);
        // A1, A3 and A5 with index 0: refused (both hosts we meet, Azahar's and the library, send 1)
        for (const char* id : {"A1", "A3", "A5"}) {
            const DlpRealGolden& g = Dialog(id);
            std::memcpy(m, g.bytes, g.size);
            m[12] = 0;
            CHECK(FromHost(m, g.size).why == Why::Index);
        }
    }
    // an A4 that wants the content (0x20 at +0x14), in the short form and in Azahar's ContentNeeded form (0x38): read as such
    {
        std::memcpy(m, Dialog("A4").bytes, kDistAckBytes);
        StoreBE16(m + 0x14, 0x0020);
        Packet r = FromClient(m, kDistAckBytes);
        CHECK(r.kind == Kind::DistAck && r.needs_content);
        std::memset(m, 0, sizeof m);
        std::memcpy(m, Dialog("A4").bytes, kDistAckBytes);
        StoreBE16(m + 4, kDistAckContentBytes);
        StoreBE16(m + 0x14, 0x0020);
        m[0x18] = 1;
        r = FromClient(m, kDistAckContentBytes);
        CHECK(r.kind == Kind::DistAck && r.needs_content);
        StoreBE16(m + 0x14, 0x0000);  // (Azahar's host reads only 0x20 as "wants it")
        r = FromClient(m, kDistAckContentBytes);
        CHECK(r.kind == Kind::DistAck && !r.needs_content);
        std::memcpy(m, Dialog("A4").bytes, kDistAckBytes);
        StoreBE16(m + 0x14, 0x0001);
        CHECK(!FromClient(m, kDistAckBytes).needs_content);
    }
    // an A6 that still downloads (+0x15 = 1)
    {
        std::memcpy(m, Dialog("A6").bytes, kFinishAckBytes);
        m[0x15] = 1;
        const Packet r = FromClient(m, kFinishAckBytes);
        CHECK(r.kind == Kind::FinishAck && r.needs_content);
    }
    // an A9 without the 9: Body
    {
        std::memcpy(m, Dialog("A9").bytes, kNoticeAckBytes);
        StoreLE32(m + 0x14, 8);
        Packet r = FromClient(m, kNoticeAckBytes);
        CHECK(r.kind == Kind::None && r.why == Why::Body);
        StoreLE32(m + 0x14, 0x09000000);  // big-endian 9 is not it
        r = FromClient(m, kNoticeAckBytes);
        CHECK(r.why == Why::Body);
    }
    // the notice with sequence 4 and 5 (an Azahar host after the first A9s): taken, and the sequence is read
    for (u8 seq : {3, 4, 5, 0, 0xFF}) {
        u8 n[kBufferBytes];
        CHECK_EQ(Notice(n, kDlpRealLuigiSeed, seq, kRid), kNoticeBytes);
        const Packet r = FromHost(n, kNoticeBytes);
        CHECK(r.kind == Kind::Notice && r.why == Why::Ok);
        CHECK_EQ(r.seq, seq);
        u8 f[kBufferBytes];
        const u8 pass[kPassphraseBytes] = {'1', '2', '3', '4', '5', '6', '7', '8', 0};
        CHECK_EQ(Final(f, kDlpRealLuigiSeed, seq, kRid, pass), kFinalBytes);
        CHECK(FromHost(f, kFinalBytes).kind == Kind::Final);
    }
    // the checksum is never checked on receipt: a golden with a broken checksum is still taken
    {
        std::memcpy(m, Dialog("A3").bytes, kStartDistBytes);
        m[8] ^= 0xFF;
        CHECK(FromHost(m, kStartDistBytes).kind == Kind::StartDist);
        std::memcpy(m, Dialog("A2").bytes, kAuthAckBytes);
        StoreBE32(m + 8, 0);
        CHECK(FromClient(m, kAuthAckBytes).kind == Kind::AuthAck);
    }
    // every prefix of every golden, with its size left and with its size made coherent, through the three parsers (ASan: no read past it)
    unsigned parsed = 0;
    auto all = [&](const DlpRealGolden& g) {
        for (std::size_t n = 0; n <= g.size; ++n) {
            std::memcpy(m, g.bytes, g.size);
            for (int coherent = 0; coherent < 2; ++coherent) {
                if (coherent && n >= 6) StoreBE16(m + 4, static_cast<u16>(n));
                const Packet a = Announce(m, n), h = FromHost(m, n), c = FromClient(m, n);
                if (n < g.size) CHECK(a.kind == Kind::None && h.kind == Kind::None && c.kind == Kind::None);
                ++parsed;
            }
        }
    };
    for (const DlpRealGolden& g : kDlpRealLuigiAnnounce) all(g);
    for (const DlpRealGolden& g : kDlpRealLuigiDialog) all(g);
    CHECK(parsed > 2 * 6576);
    // a few thousand made-up headers with every type and index over every length up to 0x40
    unsigned x = 12345;
    for (int i = 0; i < 20000; ++i) {
        x = x * 1103515245u + 12345u;
        const std::size_t n = (x >> 8) % 0x41;
        u8 r[0x40];
        for (std::size_t k = 0; k < sizeof r; ++k) {
            x = x * 1103515245u + 12345u;
            r[k] = static_cast<u8>(x >> 16);
        }
        if (n >= 6 && (x & 1)) StoreBE16(r + 4, static_cast<u16>(n));
        const Packet a = Announce(r, n), h = FromHost(r, n), c = FromClient(r, n);
        CHECK(a.kind == Kind::None || a.why == Why::Ok);
        CHECK(h.kind == Kind::None || h.why == Why::Ok);
        CHECK(c.kind == Kind::None || c.why == Why::Ok);
    }
}

// ---- W10 ----
static u32 ModelChecksum(const u8 seed[4], const u8* p, std::size_t len) {
    // written from 3dbrew "Download Play#Checksum" and Azahar's DLP_Base::GeneratePKChecksum, not from the library
    u32 sum = 0;
    for (std::size_t i = 0; i < len; i += 4) {
        u32 w = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const u8 b = (i + k < len && !(i + k >= 8 && i + k < 12)) ? p[i + k] : 0;
            w = (w << 8) | b;
        }
        sum += w;
    }
    const unsigned n = (seed[3] & 7) + 2, s = (seed[2] & 0xF) + 4;
    const u32 key = LoadBE32(seed);
    for (unsigned r = 0; r < n; ++r) sum = ((sum >> s) | (sum << s)) ^ key;
    return sum;
}

static u32 RotateChecksum(const u8 seed[4], const u8* p, std::size_t len) {  // what someone who "corrects" the step would get
    u32 sum = 0;
    for (std::size_t i = 0; i < len; i += 4) {
        u32 w = 0;
        for (std::size_t k = 0; k < 4; ++k) w = (w << 8) | ((i + k < len && !(i + k >= 8 && i + k < 12)) ? p[i + k] : 0);
        sum += w;
    }
    const unsigned n = (seed[3] & 7) + 2, s = (seed[2] & 0xF) + 4;
    for (unsigned r = 0; r < n; ++r) sum = ((sum >> s) | (sum << (32 - s))) ^ LoadBE32(seed);
    return sum;
}

static void TestChecksumEdges() {
    static const u8 kSeeds[][4] = {{0xff, 0xa4, 0x72, 0xff}, {0, 0, 0, 0}, {0x12, 0x34, 0x56, 0x78}, {0xde, 0xad, 0xbe, 0xef}, {1, 2, 0x0F, 7}};
    u8 p[0x40];
    for (std::size_t k = 0; k < sizeof p; ++k) p[k] = static_cast<u8>(0x31 * k + 7);
    for (const auto& seed : kSeeds) {
        for (std::size_t len = 0x10; len <= 0x23; ++len) {  // 0, 1, 2 and 3 trailing bytes, more than once
            auto b = Exact(p, len);
            CHECK_EQ(Checksum(seed, b.get(), len), ModelChecksum(seed, b.get(), len));
        }
    }
    // the trailing bytes go HIGH with zeros: 1 trailing byte 0xAB adds 0xAB000000
    {
        u8 a[0x11] = {0};
        u8 z[0x10] = {0};
        a[0x10] = 0xAB;
        const u8 seed[4] = {0, 0, 0, 0};
        // with seed 0 the step is invertible by hand: compare the sums through the model, and the model to the codec
        CHECK_EQ(Checksum(seed, a, sizeof a), ModelChecksum(seed, a, sizeof a));
        CHECK(Checksum(seed, a, sizeof a) != Checksum(seed, z, sizeof z));
        u8 low[0x14] = {0};
        low[0x13] = 0xAB;  // the same byte as the LOW byte of a whole word
        CHECK(Checksum(seed, a, sizeof a) != Checksum(seed, low, sizeof low));
        u8 high[0x14] = {0};
        high[0x10] = 0xAB;  // as the HIGH byte of a whole word: the same sum
        CHECK_EQ(Checksum(seed, a, sizeof a), Checksum(seed, high, sizeof high));
        u8 two[0x12] = {0}, two_w[0x14] = {0};
        two[0x10] = 0x12; two[0x11] = 0x34;
        two_w[0x10] = 0x12; two_w[0x11] = 0x34;
        CHECK_EQ(Checksum(seed, two, sizeof two), Checksum(seed, two_w, sizeof two_w));
        u8 three[0x13] = {0}, three_w[0x14] = {0};
        three[0x10] = 0x12; three[0x11] = 0x34; three[0x12] = 0x56;
        three_w[0x10] = 0x12; three_w[0x11] = 0x34; three_w[0x12] = 0x56;
        CHECK_EQ(Checksum(seed, three, sizeof three), Checksum(seed, three_w, sizeof three_w));
    }
    // one bit changed: +8..11 does not count; anywhere else the codec follows the model, and the checksum changes MOST of the time but
    // not always. (Found by this test, 2026-11-09: the step ORs two shifted copies of h, so it loses bits; with the capture's seed 355
    // single-bit changes in 768 leave the checksum as it was. It is the console's and Azahar's algorithm, and nobody on the path checks it: design 2.5.)
    unsigned flips = 0, changed = 0;
    for (const DlpRealGolden& g : kDlpRealLuigiDialog) {
        u8 m[kBufferBytes];
        std::memcpy(m, g.bytes, g.size);
        const u32 base = Checksum(kDlpRealLuigiSeed, m, g.size);
        for (std::size_t i = 0; i < g.size; ++i) {
            for (int bit = 0; bit < 8; bit += 3) {
                m[i] ^= static_cast<u8>(1u << bit);
                const u32 c = Checksum(kDlpRealLuigiSeed, m, g.size);
                CHECK_EQ(c, ModelChecksum(kDlpRealLuigiSeed, m, g.size));
                if (i >= 8 && i < 12) {
                    CHECK_EQ(c, base);
                } else {
                    ++flips;
                    if (c != base) ++changed;
                }
                m[i] ^= static_cast<u8>(1u << bit);
            }
        }
    }
    std::printf("  W10: one bit changed (outside +8..11) changed the checksum %u times in %u\n", changed, flips);
    CHECK(changed * 2 > flips);
    // the step is (h >> s) | (h << s): a rotation gives other numbers (the capture's own checksums tell them apart)
    unsigned differ = 0;
    for (const DlpRealGolden& g : kDlpRealLuigiDialog) {
        CHECK_EQ(ModelChecksum(kDlpRealLuigiSeed, g.bytes, g.size), g.checksum_capture);
        if (RotateChecksum(kDlpRealLuigiSeed, g.bytes, g.size) != g.checksum_capture) ++differ;
    }
    CHECK_EQ(differ, 13u);
    // Seal writes Checksum at +8 big-endian, and only there
    u8 s[0x18];
    std::memcpy(s, Dialog("A8").bytes, sizeof s);
    StoreBE32(s + 8, 0);
    Seal(kDlpRealLuigiSeed, s, sizeof s);
    CHECK(Same(s, Dialog("A8").bytes, sizeof s));
}

int main() {
    TestChecksums();
    TestAnnounce();
    TestHostPackets();
    TestReplies();
    TestParseGolden();
    TestParseRefusals();
    TestChecksumEdges();
    std::printf("dlp_real_wire_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
