// UDS (nwm::UDS) wire/ABI structures, as used by Azahar's HLE of the service.
#pragma once

#include "azl/common.hpp"

namespace azl {

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "ConnectionStatus assumes little endian");

constexpr std::size_t kApplicationDataSize = 0xC8;
constexpr u8 kDefaultNetworkChannel = 11;
constexpr u32 kUdsMaxNodes = 16;
constexpr u16 kBroadcastNetworkNodeId = 0xFFFF;
constexpr u16 kHostDestNodeId = 1;
constexpr u16 kNodeIdSpec = 0;
constexpr u8 kNintendoOui[3] = {0x00, 0x1F, 0x32};

// 802.11 TU is 1.024 ms; the default beacon interval is 100 TU = 102.4 ms.
constexpr u16 kDefaultBeaconInterval = 100;

// The values nwm::UDS reports. The firmware also has 0 (not initialised), 1/2 (initialising / shutting
// down) and 4/5/8 (starting a host, destroying, disconnecting: visible for ~100 ms only).
enum class NetworkStatus : u32 {
    NotConnected = 3,
    ConnectedAsHost = 6,
    Connecting = 7,
    ConnectedAsClient = 9,
    ConnectedAsSpectator = 10,
    Unavailable = 11, ///< the service was stopped (sleep, wireless off): GetConnectionStatus only
};

// Why the status last changed (the firmware's numbering; Azahar calls 4 "ConnectionLost" but it is
// DISCARDED_FROM_NETWORK, a real link loss is 5).
enum class NetworkStatusChangeReason : u32 {
    BeforeCommunication = 0,
    NetworkIsAvailable = 1,
    ConnectionEstablished = NetworkIsAvailable,
    RequestFromMyself = 2,
    RequestFromSystem = 3,
    DiscardedFromNetwork = 4,
    ConnectionLost = 5,
    Unknown = 6,
};

// Why the service refuses everything (the firmware's service-state byte; 0 = working).
enum ServiceState : u8 {
    kServiceRunning = 0,
    kServiceWirelessOff = 1, ///< the wireless switch: commands answer "wireless off"
    kServiceSleep = 2,       ///< sleep: commands answer "wrong state"
    kServiceShutdown = 3,
};

enum class ConnectionType : u8 {
    Client = 0x1,
    Spectator = 0x2,
};

// ---------------------------------------------------------------------------------------------
// NodeInfo: 40 bytes, little endian in the IPC buffers.
//   0x00 u64 friend_code_seed | 0x08 u16[10] username | 0x1C pad[4] | 0x20 u16 node id | pad[6]
// ---------------------------------------------------------------------------------------------
constexpr std::size_t kNodeInfoSize = 40;

struct NodeInfo {
    u64 friend_code_seed = 0;
    u16 username[10] = {};
    u16 network_node_id = 0;

    void Reset() {
        *this = NodeInfo{};
    }
    void Load(const u8* raw) {
        friend_code_seed = LoadLE64(raw);
        for (int i = 0; i < 10; ++i) {
            username[i] = LoadLE16(raw + 8 + i * 2);
        }
        network_node_id = LoadLE16(raw + 0x20);
    }
    void Store(u8* raw) const {
        std::memset(raw, 0, kNodeInfoSize);
        StoreLE64(raw, friend_code_seed);
        for (int i = 0; i < 10; ++i) {
            StoreLE16(raw + 8 + i * 2, username[i]);
        }
        StoreLE16(raw + 0x20, network_node_id);
    }
};

// ---------------------------------------------------------------------------------------------
// ConnectionStatus: 0x30 bytes written back to the game verbatim.
// ---------------------------------------------------------------------------------------------
struct ConnectionStatus {
    u32 status;
    u32 status_change_reason;
    u16 network_node_id;
    u16 changed_nodes;
    u16 nodes[kUdsMaxNodes];
    u8 total_nodes;
    u8 max_nodes;
    u16 node_bitmask;
};
static_assert(sizeof(ConnectionStatus) == 0x30, "ConnectionStatus has incorrect size.");

// ---------------------------------------------------------------------------------------------
// NetworkInfo: 0x108 bytes. Byte-only members so the struct has alignment 1 and can be overlaid
// on any buffer. Fields marked BE are big endian in memory.
// ---------------------------------------------------------------------------------------------
#pragma pack(push, 1)
struct NetworkInfo {
    Mac host_mac_address;
    u8 channel;
    u8 unk1;
    u8 initialized;
    u8 pad0[3];
    u8 oui_value[3];
    u8 oui_type;
    u8 wlan_comm_id_be[4]; // BE
    u8 id;
    u8 pad1;
    u8 attributes_be[2]; // BE
    u8 network_id_be[4]; // BE
    u8 total_nodes;
    u8 max_nodes;
    u8 pad2[2];
    u8 pad3[0x1F];
    u8 application_data_size;
    u8 application_data[kApplicationDataSize];

    u32 wlan_comm_id() const {
        return LoadBE32(wlan_comm_id_be);
    }
    u32 network_id() const {
        return LoadBE32(network_id_be);
    }
};
#pragma pack(pop)
static_assert(sizeof(NetworkInfo) == 0x108, "NetworkInfo has incorrect size.");
static_assert(offsetof(NetworkInfo, oui_value) == 0xC, "oui_value is at the wrong offset.");
static_assert(offsetof(NetworkInfo, wlan_comm_id_be) == 0x10, "wlancommid is at the wrong offset.");
static_assert(offsetof(NetworkInfo, application_data_size) == 0x3F, "appdata size offset");

// ---------------------------------------------------------------------------------------------
// Result codes (same encoding as Azahar's ResultCode). The set below is the one nwm::UDS itself
// answers with, taken from the firmware; the names say what the firmware means, not what Azahar
// calls them.
// ---------------------------------------------------------------------------------------------
namespace res {

enum Description : u32 {
    NetworkFull = 1,
    WirelessOff = 2,
    TooLarge = 1001,
    NotAuthorized = 1002,
    AggregateFull = 1008,
    OutOfMemory = 1011,
    NotFound = 1018,
    ConnectionFailed = 1019,
    BadArgument = 1021,
    Timeout = 1022,
};
enum Summary : u32 {
    OutOfResource = 3,
    NotFoundSummary = 4,
    InvalidState = 5,
    WrongArgument = 8,
    Canceled = 9,
    StatusChanged = 10,
};
enum Level : u32 {
    Status = 25,
    Usage = 28,
};
constexpr u32 kModuleUDS = 68;

constexpr u32 Make(u32 description, u32 summary, u32 level) {
    return description | (kModuleUDS << 10) | (summary << 21) | (level << 27);
}

constexpr u32 kSuccess = 0;

constexpr u32 kWrongState = Make(NotAuthorized, InvalidState, Status);        // 0xC8A113EA
constexpr u32 kWirelessOff = Make(WirelessOff, StatusChanged, Status);    // 0xC9411002
constexpr u32 kBadId = Make(NotAuthorized, WrongArgument, Usage);             // 0xE10113EA
constexpr u32 kTooLarge = Make(TooLarge, WrongArgument, Usage);               // 0xE10113E9
constexpr u32 kBadArgument = Make(BadArgument, WrongArgument, Usage);         // 0xE10113FD
constexpr u32 kOutOfMemory = Make(OutOfMemory, OutOfResource, Status);        // 0xC86113F3
constexpr u32 kAggregateFull = Make(AggregateFull, OutOfResource, Status);    // 0xC86113F0
constexpr u32 kNodeNotFound = Make(NotFound, WrongArgument, Status);          // 0xC90113FA
constexpr u32 kNoHostFound = Make(NotFound, NotFoundSummary, Status);         // 0xC88113FA
constexpr u32 kConnectionFailed = Make(ConnectionFailed, Canceled, Status);   // 0xC92113FB
constexpr u32 kConnectTimeout = Make(Timeout, Canceled, Status);              // 0xC92113FE
constexpr u32 kNetworkFull = Make(NetworkFull, OutOfResource, Status);        // 0xC8611001
constexpr u32 kInvalidBeacon = 0xE1211005;
/// What the service dispatcher answers (reply header 0x40) for a command id it does not have.
constexpr u32 kUnknownCommand = 0xD900182F;

} // namespace res

} // namespace azl
