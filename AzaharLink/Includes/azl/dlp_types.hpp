// Types of the fake Download Play lobby (design: analysis/dlp-lobby-fake-2026-10-28/DESIGN-FASE1.md).
//
// What this header holds: the sizes and OFFSETS of the three records a game reads from dlp:FKCL / dlp:SRVR (no struct
// is laid over a buffer of the game: the writer stores field by field with StoreLE*), the state numbers, the module
// result codes, the constexpr helpers of the IPC headers and descriptors, and the byte formats of the virtual network
// (the beacon announcement and the four messages) with their codecs.
//
// HARD LIMITS (rules of the user, 28/10): THERE IS NEVER A FIFTH PLAYER. Everything is sized for host + 3 clients and
// says so with a static_assert, here on the constants and in dlp_lobby.hpp on the tables. No heap, no state, no log.
#pragma once

#include "azl/common.hpp"

namespace azl {

// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)

// ---------------------------------------------------------------------------------------------------------------
// Limits.

constexpr u32 kMaxPlayers = 4;  // host + 3 clients: THERE IS NEVER A FIFTH
constexpr u32 kMaxClients = 3;  // clients of one host
constexpr u32 kMaxGroups = 4;   // lobbies a searcher keeps (a room of 4 players holds at most 4 hosts)
constexpr u32 kNameUnits = 10;  // UTF-16 units of a player name
static_assert(kMaxPlayers == 4, "the product never has more than 4 players: do not raise this");
static_assert(kMaxClients + 1 == kMaxPlayers, "host + clients = players");
constexpr u32 kFirstClientNode = 2;  // the host is node 1; the clients are nodes 2..4
static_assert(kFirstClientNode + kMaxClients - 1 == kMaxPlayers, "the last client is the last player");
static_assert(kMaxGroups <= kMaxPlayers, "in a room of kMaxPlayers there are at most kMaxPlayers lobbies");

// ---------------------------------------------------------------------------------------------------------------
// The records of the game (little endian, natural alignment of the module). Offsets, not structs.

constexpr u32 kTitleInfoSize = 0x13A8;      // DLPTitleInfo (5032 bytes)
constexpr u32 kTitleInfoUniqueId = 0x00;    // u32: child title id low >> 8
constexpr u32 kTitleInfoVariation = 0x04;   // u32: the child index in the low byte
constexpr u32 kTitleInfoMac = 0x08;         // 6 bytes
constexpr u32 kTitleInfoVersion = 0x0E;     // u16
constexpr u32 kTitleInfoAgeRatings = 0x10;  // 16 bytes
constexpr u32 kTitleInfoTitleShort = 0x20;  // 64 UTF-16 units
constexpr u32 kTitleInfoTitleLong = 0xA0;   // 128 UTF-16 units
constexpr u32 kTitleInfoIcon = 0x1A0;       // 48x48 RGB565
constexpr u32 kTitleInfoIconSize = 0x1200;
constexpr u32 kTitleInfoSizeField = 0x13A0; // u32
static_assert(kTitleInfoTitleShort + 64 * 2 == kTitleInfoTitleLong, "title_short is 128 bytes");
static_assert(kTitleInfoTitleLong + 128 * 2 == kTitleInfoIcon, "title_long is 256 bytes");
static_assert(kTitleInfoIcon + kTitleInfoIconSize == kTitleInfoSizeField, "the icon is 4608 bytes");
static_assert(kTitleInfoSizeField + 8 == kTitleInfoSize, "size + unk2/unk3 + padding close the record");

constexpr u32 kDlpNodeInfoSize = 0x28;      // DLPNodeInfo (40 bytes). (Named "Dlp": uds_types.hpp has a kNodeInfoSize of its own in this namespace.)
constexpr u32 kNodeInfoSeed = 0x00;         // u64 friend code seed
constexpr u32 kNodeInfoPad = 0x08;          // 4 bytes, zero
constexpr u32 kNodeInfoName = 0x0C;         // 10 UTF-16 units (confirmed by Triple Deluxe)
constexpr u32 kNodeInfoUnk1 = 0x20;         // u32, zero
constexpr u32 kNodeInfoNodeId = 0x24;       // u32 network node id (confirmed by Triple Deluxe)
static_assert(kNodeInfoPad + 4 == kNodeInfoName, "pad is 4 bytes");
static_assert(kNodeInfoName + kNameUnits * 2 == kNodeInfoUnk1, "the name is 20 bytes");
static_assert(kNodeInfoNodeId + 4 == kDlpNodeInfoSize, "the node id closes the record");

constexpr u32 kServerInfoSize = 0x298;      // DLPServerInfo (664 bytes)
constexpr u32 kServerInfoMac = 0x00;        // 6 bytes
constexpr u32 kServerInfoUnk1 = 0x06;       // u8, zero
constexpr u32 kServerInfoSignal = 0x07;     // u8, 0..3 bars
constexpr u32 kServerInfoMaxClients = 0x08; // u8: max_nodes (host included, Azahar's convention, copied on purpose)
constexpr u32 kServerInfoJoined = 0x09;     // u8: players now (host included: the game hides a group with +9 == +8)
constexpr u32 kServerInfoUnk3 = 0x0A;       // u16, zero
constexpr u32 kServerInfoNodes = 0x10;      // 16 x DLPNodeInfo
constexpr u32 kServerInfoNodeCount = 16;
constexpr u32 kServerInfoHostName = kServerInfoNodes + kNodeInfoName;  // node_info[0].username = 0x1C
constexpr u32 kServerInfoTick = 0x290;      // u64: the system tick the entry was last heard at
static_assert(kServerInfoNodes + kServerInfoNodeCount * kDlpNodeInfoSize == kServerInfoTick, "16 nodes end at +0x290");
static_assert(kServerInfoTick + 8 == kServerInfoSize, "the tick closes the record");
static_assert(kServerInfoHostName == 0x1C, "the host name is read at +0x1C");

constexpr u32 kEventDescSize = 0x18;        // DLPEventDescription (24 bytes of zeros)

// ---------------------------------------------------------------------------------------------------------------
// States. The searcher is dlp:FKCL, the creator is dlp:SRVR. Numbers as the games read them.

constexpr u8 kCliOff = 0;        // not initialised
constexpr u8 kCliIdle = 1;       // initialised, outside any lobby
constexpr u8 kCliScanning = 2;
constexpr u8 kCliJoining = 3;    // internal: the game keeps seeing 1 while the join is in progress
constexpr u8 kCliInSession = 5;  // joined
constexpr u8 kCliStarted = 6;    // the creator started the distribution ("downloading")
constexpr u8 kCliComplete = 9;   // the passphrase arrived: final, only Finalize / StopSession / Initialize leave it
constexpr u8 kCliLost = 0x80;    // the host vanished in 5 or 6 (never 7)

constexpr u8 kSrvOff = 0;
constexpr u8 kSrvIdle = 1;       // initialised (also after EndAccepting)
constexpr u8 kSrvAccepting = 2;
constexpr u8 kSrvDistributing = 6;
constexpr u8 kSrvPassphrase = 8; // the game generates the passphrase and sends it, every frame
constexpr u8 kSrvComplete = 9;

constexpr u8 kPeerNotJoined = 1;     // GetClientState (external numbers)
constexpr u8 kPeerWaiting = 0x40;    // joined the network, not yet accepted
constexpr u8 kPeerAccepted = 5;
constexpr u8 kPeerStarting = 6;      // the distribution started, no answer yet
constexpr u8 kPeerDistributed = 7;
constexpr u8 kPeerComplete = 9;

// ---------------------------------------------------------------------------------------------------------------
// Module results (module 43, DLP). Success is 0. No placeholder -1 anywhere.

constexpr u32 kInvalidState = 0xE0A0AC01;    // a command out of its state
constexpr u32 kBusy = 0xE0A0AC03;            // the module / the radio is taken by the other role or the engine
constexpr u32 kNoData = 0xC880AFEF;          // MAC or node unknown, end of list
constexpr u32 kNotFound = 0xC880AC05;        // "server not found" (the join could not start or was refused)
constexpr u32 kTimeout = 0xC920AFFE;         // the join did not finish in time
constexpr u32 kOutOfRange = 0xD8E0AFFD;      // StartAccepting channel not in {0, 1, 6, 11}
constexpr u32 kInvalidPointer = 0xD8E0AFF6;  // a buffer of the game could not be used
constexpr u32 kBadHeader = 0xD9001830;       // an id the service does not have (the answer of DlpFkclHle)

// ---------------------------------------------------------------------------------------------------------------
// IPC helpers (libctru's macros do not exist in the pure tests).

constexpr u32 Header(u32 id, u32 normal, u32 translate) {
    return (id << 16) | (normal << 6) | translate;
}
constexpr u32 StaticBufferDesc(u32 size, u32 id) {
    return (size << 14) | ((id & 0xF) << 10) | 2;
}
constexpr u32 MappedWriteDesc(u32 bytes) {
    return (bytes << 4) | 0xC;
}

// ---------------------------------------------------------------------------------------------------------------
// The virtual network: who we are, the announcement of a lobby, what a searcher hears.

struct Identity {  // who we are in the virtual network (the engine puts it in our NodeInfo)
    u64 seed;      // friend code seed (unique per console, chosen by the adapter; 0 = unknown)
    u16 name[kNameUnits];
};
struct NodeEntry {  // one node of the virtual network as the engine knows it (32 bytes on every ABI)
    u64 seed;
    u16 name[kNameUnits];
    u8 node;        // network node id, 1 = the host
};
struct HostAnnounce {  // what the creator puts in the beacon's application data
    u32 child_tid_low; // (game title id low32 & 0xFFFFFF00) | child index
    u16 name[kNameUnits];
    u8 max_nodes;      // max clients + 1, 2..4
    u8 accepting;      // 1 while new nodes are admitted
};
// (Deviation: the fields are in this order, not the design's, to make it 36 bytes instead of 40: the largest local of the
// module, held by the scan loop. Initialise it by field name, not positionally.)
struct HeardGroup {    // a lobby heard by the searcher (36 bytes: the largest local of the module)
    Mac host;          // transmitter of the beacon = the MAC the creator's nwm::UDS will use later
    u8 total_nodes;    // nodes in that virtual network now, host included (from the beacon)
    u8 signal;         // 0..3 bars
    HostAnnounce announce;
};
static_assert(sizeof(HeardGroup) <= 40, "a HeardGroup lives on the stack of a game thread");

// ---------------------------------------------------------------------------------------------------------------
// Wire formats (version 1, little endian, no checksum: the channel is the data of a protected UDS network).
//
// Announcement, 29 bytes: 'D' 'L' | u8 version = 1 | u8 flags (bit0 = accepting) | u32 child_tid_low | u8 max_nodes |
// u16[10] name. `total_nodes` is NOT in it: the beacon (the engine's NetworkInfo) carries it.
// Message, 2 + length bytes: u8 type | u8 body length | body.

constexpr u32 kAnnounceSize = 29;
constexpr u32 kMaxMessage = 16;
constexpr u32 kPassphraseSize = 9;  // the 8 characters of the game's "%08x" + NUL, opaque to us
constexpr u8 kAnnounceVersion = 1;
static_assert(kAnnounceSize == 9 + kNameUnits * 2, "29 = 9 fixed bytes + the 20-byte name");
static_assert(2 + kPassphraseSize <= kMaxMessage, "the largest message (GO) fits in kMaxMessage");

enum class MsgType : u8 { Start = 1, StartAck = 2, Go = 3, GoAck = 4 };

/// Writes the announcement. Cost: 24 bytes of stack. Edge cases: any field value is written as is (max_nodes is not
/// checked here: DecodeAnnounce refuses a bad one at the other end).
inline void EncodeAnnounce(const HostAnnounce& a, u8 out[kAnnounceSize]) {
    out[0] = 'D';
    out[1] = 'L';
    out[2] = kAnnounceVersion;
    out[3] = a.accepting ? 1 : 0;
    StoreLE32(out + 4, a.child_tid_low);
    out[8] = a.max_nodes;
    for (u32 i = 0; i < kNameUnits; ++i) {
        StoreLE16(out + 9 + 2 * i, a.name[i]);
    }
}

/// Reads and validates an announcement. Returns false (and leaves *out untouched) for: null data, size < 29, magic or
/// version different, max_nodes outside 2..4. A longer announcement with version 1 is accepted (later fields are
/// ignored). Cost: 16 bytes of stack. Never reads beyond `size`.
inline bool DecodeAnnounce(const u8* data, u32 size, HostAnnounce* out) {
    if (data == nullptr || out == nullptr || size < kAnnounceSize) {
        return false;
    }
    if (data[0] != 'D' || data[1] != 'L' || data[2] != kAnnounceVersion) {
        return false;
    }
    if (data[8] < 2 || data[8] > kMaxPlayers) {
        return false;
    }
    out->accepting = (data[3] & 1) ? 1 : 0;
    out->child_tid_low = LoadLE32(data + 4);
    out->max_nodes = data[8];
    for (u32 i = 0; i < kNameUnits; ++i) {
        out->name[i] = LoadLE16(data + 9 + 2 * i);
    }
    return true;
}

/// The body length a message type must carry, or 0xFF for an unknown type.
inline u32 MessageBodySize(u8 type) {
    switch (type) {
    case static_cast<u8>(MsgType::Start):
    case static_cast<u8>(MsgType::StartAck):
    case static_cast<u8>(MsgType::GoAck):
        return 0;
    case static_cast<u8>(MsgType::Go):
        return kPassphraseSize;
    default:
        return 0xFF;
    }
}

/// Writes a message and returns its size (2 + body_len), or 0 on error: unknown type, body_len different from the
/// one the type carries, a null body with a length. Cost: 16 bytes of stack. Writes nothing on error.
inline u32 EncodeMessage(MsgType type, const u8* body, u32 body_len, u8 out[kMaxMessage]) {
    const u32 want = MessageBodySize(static_cast<u8>(type));
    if (want == 0xFF || body_len != want || (body_len != 0 && body == nullptr)) {
        return 0;
    }
    out[0] = static_cast<u8>(type);
    out[1] = static_cast<u8>(body_len);
    for (u32 i = 0; i < body_len; ++i) {
        out[2 + i] = body[i];
    }
    return 2 + body_len;
}

/// (An addition to the design's codec: the receiving frame of the lobby uses it instead of DecodeMessage.)
/// True if `data` is a well-formed message: size >= 2, a known type, the length byte the type's, size == 2 + length. The same
/// test as DecodeMessage without the out-parameters (the receiving frame of the lobby keeps no spare locals). Never reads
/// beyond `size`. No stack.
inline bool IsMessage(const u8* data, u32 size) {
    if (data == nullptr || size < 2) {
        return false;
    }
    const u32 want = MessageBodySize(data[0]);
    return want != 0xFF && data[1] == want && size == 2 + want;
}

/// Reads and validates a message. Returns false for: null data, size < 2, unknown type, length byte different from the
/// type's, size != 2 + length. *body points into `data`. Never reads beyond `size`. Cost: 16 bytes of stack.
inline bool DecodeMessage(const u8* data, u32 size, MsgType* type, const u8** body) {
    if (data == nullptr || type == nullptr || body == nullptr || size < 2) {
        return false;
    }
    const u32 want = MessageBodySize(data[0]);
    if (want == 0xFF || data[1] != want || size != 2 + want) {
        return false;
    }
    *type = static_cast<MsgType>(data[0]);
    *body = data + 2;
    return true;
}

// <<< game-thread code

}  // namespace azl
