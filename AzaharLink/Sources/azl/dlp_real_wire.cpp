// The wire of the real Download Play (azl/dlp_real_wire.hpp). The host's packets A1, A3, A5 and A7 are the library's (dlph_wire.c, the
// same bytes as Azahar's: design 2.4); the notice A8, the announcement in our form and the fake client's answers are written here.
#include "azl/dlp_real_wire.hpp"

#include "../../ConfigApp/dlph/dlph_wire.h"  // (one file of the library, by name: the Makefile builds dlph_wire.c and nothing else of ConfigApp/dlph)

namespace azl {
namespace dlp_real {

static_assert(kBufferBytes == DLPH_MAX_FRAME, "a buffer of this codec is what the library's builders write into");
static_assert(kFrame0Bytes == DLPH_BC0_LEN && kIconFrameBytes == DLPH_BCI_LEN && kFrame4Bytes == DLPH_BC4_LEN, "the frames of the announcement");
static_assert(kAuthBytes == 0x14 && kStartDistBytes == 0x14 && kFinishBytes == 0x18 && kFinalBytes == 0x20, "what dlph_pk_* return");

namespace {

constexpr u8 kTypeAnnounce = 1, kTypeAuth = 2, kTypeStartDist = 3, kTypeFinish = 5, kTypeBegin = 6;

void Header(u8* o, u8 type, std::size_t len, u8 index, u8 seq, u8 rid0, u8 rid1) {
    std::memset(o, 0, len);
    o[0] = type;
    o[1] = 0x02;
    StoreBE16(o + 4, static_cast<u16>(len));
    o[6] = 0x02;
    o[12] = index;
    o[13] = seq;
    o[14] = rid0;
    o[15] = rid1;
}

std::size_t FrameBytes(unsigned index) {
    return index == 0 ? kFrame0Bytes : index < 4 ? kIconFrameBytes : kFrame4Bytes;
}

/// The header's checks: long enough for it, and the length it says is the length received.
Packet Head(const u8* p, std::size_t len) {
    Packet r{};
    if (len < kHeaderBytes) {
        r.why = Why::Short;
        return r;
    }
    r.index = p[12];
    r.seq = p[13];
    r.rid[0] = p[14];
    r.rid[1] = p[15];
    if (LoadBE16(p + 4) != len) r.why = Why::Size;
    return r;
}

Packet Take(Packet r, Kind kind, std::size_t len, std::size_t want, int want_index) {
    if (want_index >= 0 && r.index != want_index) {
        r.why = Why::Index;
    } else if (len != want) {
        r.why = Why::Length;
    } else {
        r.kind = kind;
    }
    return r;
}

}  // namespace

u32 Checksum(const u8 seed[4], const u8* p, std::size_t len) {
    return dlph_checksum(seed, p, len);
}

void Seal(const u8 seed[4], u8* p, std::size_t len) {
    dlph_seal(seed, p, len);
}

std::size_t AnnounceFrame(u8* buf, unsigned index, u64 child_tid, u8 max_nodes, const u8 seed[4]) {
    if (index >= kAnnounceFrames) return 0;
    const std::size_t len = FrameBytes(index);
    Header(buf, kTypeAnnounce, len, static_cast<u8>(index), static_cast<u8>(kAnnounceFrames), 0, 0);
    if (index == 0) {
        StoreBE64(buf + 0x10, child_tid);
        StoreBE16(buf + 0x18, 0x0010);
        buf[0x1A] = 0x01;
        buf[0x1B] = 0x01;
        StoreBE32(buf + 0x1C, DLPH_CHUNK_SIZE);
        buf[0x20] = max_nodes;
    }
    Seal(seed, buf, len);
    return len;
}

std::size_t Auth(u8* buf, const u8 seed[4]) {
    return dlph_pk_auth(buf, seed);
}

std::size_t StartDist(u8* buf, const u8 seed[4], u8 seq, const u8 rid[2]) {
    return dlph_pk_startdist(buf, seed, seq, rid);
}

std::size_t Finish(u8* buf, const u8 seed[4], u8 seq, const u8 rid[2]) {
    return dlph_pk_finish_req(buf, seed, seq, rid, 0);
}

std::size_t Final(u8* buf, const u8 seed[4], u8 seq, const u8 rid[2], const u8 pass[kPassphraseBytes]) {
    return dlph_pk_begin_final(buf, seed, seq, rid, pass);
}

std::size_t Notice(u8* buf, const u8 seed[4], u8 seq, const u8 rid[2]) {
    // Azahar's DLPSrvr_BeginGame: index 0, +0x10 = 1 and +0x14 = 9, little-endian (NOT the library's begin_ping: index 1, 0x20 bytes,
    // +0x1D = 8, which an Azahar client takes for the final with an empty passphrase; design 1 and 2.4)
    Header(buf, kTypeBegin, kNoticeBytes, 0, seq, rid[0], rid[1]);
    StoreLE32(buf + 0x10, 1);
    StoreLE32(buf + 0x14, 9);
    Seal(seed, buf, kNoticeBytes);
    return kNoticeBytes;
}

std::size_t Reply(u8* buf, const u8 seed[4], const Packet& got) {
    std::size_t len = 0;
    u8 type = 0;
    switch (got.kind) {
    case Kind::Auth: len = kAuthAckBytes; type = kTypeAuth; break;
    case Kind::StartDist: len = kDistAckBytes; type = kTypeStartDist; break;
    case Kind::Finish: len = kFinishAckBytes; type = kTypeFinish; break;
    case Kind::Notice: len = kNoticeAckBytes; type = kTypeBegin; break;
    default: return 0;
    }
    Header(buf, type, len, 0, got.seq, got.rid[0], got.rid[1]);
    StoreLE32(buf + 0x10, 1);  // "initialized" / unk1
    if (got.kind == Kind::Auth) {
        buf[0x16] = 0x01;  // the client's id: Azahar's fixed 01 02 (dlp_clt_base.cpp:695-697)
        buf[0x17] = 0x02;
    } else if (got.kind == Kind::Finish) {
        buf[0x14] = 1;     // finished_cur_block; needs_content (+0x15) and seq_ack (+0x16..19) stay 0
    } else if (got.kind == Kind::Notice) {
        StoreLE32(buf + 0x14, 9);
    }                      // (StartDist: +0x14 = 00 00, no content needed)
    Seal(seed, buf, len);
    return len;
}

Packet ParseAnnounce(const u8* p, std::size_t len) {
    Packet r = Head(p, len);
    if (r.why != Why::Ok) return r;
    if (p[0] != kTypeAnnounce) {
        r.why = Why::Type;
    } else if (r.index >= kAnnounceFrames) {
        r.why = Why::Index;
    } else {
        r = Take(r, Kind::Announce, len, FrameBytes(r.index), -1);
    }
    return r;
}

Packet ParseFromHost(const u8* p, std::size_t len) {
    Packet r = Head(p, len);
    if (r.why != Why::Ok) return r;
    switch (p[0]) {
    case kTypeAuth: return Take(r, Kind::Auth, len, kAuthBytes, 1);
    case kTypeStartDist: return Take(r, Kind::StartDist, len, kStartDistBytes, 1);
    case kTypeFinish: return Take(r, Kind::Finish, len, kFinishBytes, 1);
    case kTypeBegin:
        if (r.index == 0) return Take(r, Kind::Notice, len, kNoticeBytes, 0);
        return Take(r, Kind::Final, len, kFinalBytes, 1);
    default: r.why = Why::Type; return r;
    }
}

Packet ParseFromClient(const u8* p, std::size_t len) {
    Packet r = Head(p, len);
    if (r.why != Why::Ok) return r;
    switch (p[0]) {
    case kTypeAuth:
        r = Take(r, Kind::AuthAck, len, kAuthAckBytes, -1);
        if (r.kind == Kind::AuthAck) {  // the client's id (+0x16..17), which the host puts in every later packet
            r.rid[0] = p[0x16];
            r.rid[1] = p[0x17];
        }
        return r;
    case kTypeStartDist:
        r = Take(r, Kind::DistAck, len, len == kDistAckContentBytes ? kDistAckContentBytes : kDistAckBytes, -1);
        if (r.kind == Kind::DistAck) r.needs_content = LoadBE16(p + 0x14) == 0x0020;
        return r;
    case kTypeFinish:
        r = Take(r, Kind::FinishAck, len, kFinishAckBytes, -1);
        if (r.kind == Kind::FinishAck) r.needs_content = p[0x15] != 0;
        return r;
    case kTypeBegin:
        r = Take(r, Kind::NoticeAck, len, kNoticeAckBytes, -1);
        if (r.kind == Kind::NoticeAck && LoadLE32(p + 0x14) != 9) {
            r.kind = Kind::None;
            r.why = Why::Body;
        }
        return r;
    default: r.why = Why::Type; return r;
    }
}

u64 AnnounceChildTid(const u8* frame0) {
    return LoadBE64(frame0 + 0x10);
}

u8 AnnounceMaxNodes(const u8* frame0) {
    return frame0[0x20];
}

}  // namespace dlp_real
}  // namespace azl
