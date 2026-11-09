// The wire of the REAL Download Play (1.1.3, DlpGameMode::RealDlp): the bytes a Download Play host and a "fake client" (every
// player has the game, nothing is downloaded) put in SecureData frames, as Azahar's DLP service speaks them. Design:
// analysis/luigi-azahar-2026-11-09/PROGETTO-DLP-VERO-1.1.3.md, section 2 (the tables there are this codec's contract); the golden
// bytes are Tests/dlp_real_luigi_golden.inc (the capture 1c, two Azahar emulators, Luigi's Mansion 2018) and Tests/dlp_real_wire_test.cpp
// holds the codec to them (W1..W10 of the design).
//
// Every packet starts with a header of 16 bytes: +0 type, +1 0x02, +2..3 zero, +4..5 the WHOLE packet's length (big-endian),
// +6..7 02 00, +8..11 the checksum (big-endian), +0x0C the packet index, +0x0D..0x0F the host's sequence and the client's two-byte id.
//   channel 1, host -> 0xFFFF:  the announcement, a group of 5 frames (768, 1448, 1448, 1448, 1464 bytes), only while a spectator is there
//   channel 2, host -> client:  Auth (A1), StartDistribution (A3), FinishContentUpload (A5), the final with the passphrase (A7), the notice (A8)
//   channel 2, client -> host:  the answers A2 (to A1), A4 (to A3), A6 (to A5), A9 (to A8); the final A7 is not answered
//
// THE CHECKSUM. The library's (DlpLink/host/dlph_wire.c, built into the plugin by name: Makefile): the big-endian words summed, the
// 1..3 trailing bytes high with zeros, the field at +8 counted as zero; then cnt = (seed[3] & 7) + 2 rounds of
// h = ((h >> s) | (h << s)) ^ BE32(seed) with s = (seed[2] & 0xF) + 4. That step is NOT a rotation (the same s both ways, not 32 - s):
// it is what Azahar and the console compute, do not "correct" it. What we send is sealed with kTxSeed (design 2.5: nobody on the path
// checks it, Azahar never does: should_verify_checksum = false); what we receive is never checked.
//
// No state, no allocation, no log: pure functions over the caller's buffer (kBufferBytes). The parsers read nothing past `len`.
#pragma once

#include "azl/common.hpp"

namespace azl {
namespace dlp_real {

constexpr std::size_t kHeaderBytes = 0x10;
constexpr unsigned kAnnounceFrames = 5;
constexpr std::size_t kFrame0Bytes = 768;     // frame 0: the title's numbers (+ titles and icon, zero in our frame: design 2.2, option c)
constexpr std::size_t kIconFrameBytes = 1448;  // frames 1..3: the rest of the icon (zero in ours)
constexpr std::size_t kFrame4Bytes = 1464;     // frame 4: zero in Azahar's and in ours
constexpr std::size_t kAnnounceGroupBytes = kFrame0Bytes + 3 * kIconFrameBytes + kFrame4Bytes;
static_assert(kAnnounceGroupBytes == 6576, "a group of the announcement is 6576 bytes (design 2.2)");
/// What a buffer of this codec must hold: the biggest frame it writes (1464) rounded to the library's DLPH_MAX_FRAME.
constexpr std::size_t kBufferBytes = 1468;

// channel 2 lengths: the capture's (design 2.3). Azahar takes a SHORTER packet with a coherent size for a null body and crashes.
constexpr std::size_t kAuthBytes = 0x14;            // A1
constexpr std::size_t kStartDistBytes = 0x14;       // A3
constexpr std::size_t kFinishBytes = 0x18;          // A5
constexpr std::size_t kFinalBytes = 0x20;           // A7
constexpr std::size_t kNoticeBytes = 0x18;          // A8
constexpr std::size_t kAuthAckBytes = 0x18;         // A2
constexpr std::size_t kDistAckBytes = 0x18;         // A4 (no content needed)
constexpr std::size_t kDistAckContentBytes = 0x38;  // A4 of a client that wants the child (a download: not this mode)
constexpr std::size_t kFinishAckBytes = 0x1C;       // A6
constexpr std::size_t kNoticeAckBytes = 0x18;       // A9
constexpr std::size_t kPassphraseAt = 0x14;         // in the final A7: the game's passphrase, 9 bytes (8 characters and a zero)
constexpr std::size_t kPassphraseBytes = 9;

/// The seed of the checksum of everything WE send (design 2.5): a constant, a parameter of every builder.
inline constexpr u8 kTxSeed[4] = {0, 0, 0, 0};
/// The child's title id: 0x00040001 above the low word that the lobby computes (HostAnnounce::child_tid_low).
constexpr u64 kChildTidHigh = 0x0004000100000000ULL;

enum class Kind : u8 {
    None = 0,   // rejected (see Why)
    Announce,   // channel 1, a frame of the group (index 0..4)
    Auth,       // A1   host -> client
    StartDist,  // A3
    Finish,     // A5
    Notice,     // A8   (idx 0; also before the final)
    Final,      // A7   (idx 1, the passphrase)
    AuthAck,    // A2   client -> host
    DistAck,    // A4   (needs_content: the client wants the child)
    FinishAck,  // A6   (needs_content: the client is still downloading)
    NoticeAck,  // A9   (+0x14 = 9)
};

enum class Why : u8 {
    Ok = 0,
    Short,   // shorter than the header
    Size,    // the length at +4 is not the length received
    Type,    // a type this side does not take
    Index,   // the packet index is not the one of its kind (or >= 5 for a frame of the announcement)
    Length,  // not the length of its kind (or of its frame)
    Body,    // a field of the body is not what its kind needs (A9 without the 9)
};

/// What a parser read. seq and rid are +0x0D, +0x0E..0F of the header: a client copies them into its answer. For an AuthAck (A2)
/// rid is the client's id from the body (+0x16..17), the one the host then puts in every packet to that client.
struct Packet {
    Kind kind;
    Why why;
    u8 index;
    u8 seq;
    u8 rid[2];
    bool needs_content;
};

u32 Checksum(const u8 seed[4], const u8* p, std::size_t len);
/// Writes the checksum at +8..11.
void Seal(const u8 seed[4], u8* p, std::size_t len);

/// Frame `index` (0..4) of the announcement in `buf` (kBufferBytes), zeroed first and sealed; returns its length, 0 for an index >= 5.
/// Frame 0: +0x10 child_tid, +0x18 00 10, +0x1A 01 01, +0x1C 00 03 ff c0, +0x20 max_nodes, the rest zero (no title, no icon, no sizes).
std::size_t AnnounceFrame(u8* buf, unsigned index, u64 child_tid, u8 max_nodes, const u8 seed[4]);

// host -> client, channel 2. `buf` holds kBufferBytes. Each returns the length.
std::size_t Auth(u8* buf, const u8 seed[4]);
std::size_t StartDist(u8* buf, const u8 seed[4], u8 seq, const u8 rid[2]);
std::size_t Finish(u8* buf, const u8 seed[4], u8 seq, const u8 rid[2]);
std::size_t Final(u8* buf, const u8 seed[4], u8 seq, const u8 rid[2], const u8 pass[kPassphraseBytes]);
std::size_t Notice(u8* buf, const u8 seed[4], u8 seq, const u8 rid[2]);

/// The fake client's answer to what the host sent (A2 to an Auth, A4 to a StartDist, A6 to a Finish, A9 to a Notice): index 0, the
/// host's +0x0D..0F copied, no content needed. Returns the length, 0 for a kind that is not answered (the final, a rejected packet).
std::size_t Reply(u8* buf, const u8 seed[4], const Packet& got);

/// A frame of channel 1: type 1, the length of its index. Never reads past `len`.
Packet ParseAnnounce(const u8* p, std::size_t len);
/// What a fake client takes from its host on channel 2 (design 5.4). The sequence is not checked (an Azahar host counts on).
Packet ParseFromHost(const u8* p, std::size_t len);
/// What the host takes from a client on channel 2 (design 4.4). The index is not checked (Azahar's host does not).
Packet ParseFromClient(const u8* p, std::size_t len);

/// Fields of a frame 0 that ParseAnnounce accepted (index 0, kFrame0Bytes).
u64 AnnounceChildTid(const u8* frame0);
u8 AnnounceMaxNodes(const u8* frame0);

}  // namespace dlp_real
}  // namespace azl
