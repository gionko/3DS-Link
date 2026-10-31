// Synthetic 802.11 frames exchanged through the Azahar room (port of uds_beacon/uds_data/
// uds_connection from Azahar).
#pragma once

#include <vector>

#include "azl/common.hpp"
#include "azl/uds_types.hpp"

namespace azl {

/// The unit relayed by the room server. Mirrors Network::WifiPacket in Azahar.
struct WifiPacket {
    enum class Type : u8 {
        Beacon = 0,
        Data,
        Authentication,
        AssociationResponse,
        Deauthentication,
        NodeMap,
    };
    Type type = Type::Data;
    std::vector<u8> data;
    Mac transmitter_address{};
    Mac destination_address{};
    u8 channel = 0;
    bool relayed = false; ///< a host's copy of a frame for another node (not on the wire: the room client's delivery rule treats it as unicast)
    bool peer_numbers = false; ///< the destination node numbers its frames (it runs this plugin: it drops a copy); not on the wire
    bool broadcast_flag = false; ///< the game sent it with the broadcast flag (SendTo flags & 2): the radio does not retry it, whatever MAC it is addressed to; not on the wire
};

enum class EtherType : u16 { SecureData = 0x876D, EAPoL = 0x888E };

constexpr std::size_t kLlcHeaderSize = 8;
constexpr std::size_t kSecureDataHeaderSize = 14;
constexpr u16 kEapolStartMagic = 0x201;
constexpr u16 kEapolLogoffMagic = 0x202;
constexpr std::size_t kEapolNodeInfoSize = 0x28;
constexpr std::size_t kEapolStartSize = 0x30;
constexpr std::size_t kEapolLogoffSize = 0x298;

// Sizes used when scanning beacons for the game (BeaconDataReplyHeader / BeaconEntryHeader).
constexpr std::size_t kBeaconDataReplyHeaderSize = 12;
constexpr std::size_t kBeaconEntryHeaderSize = 0x1C;

// ---- Beacon -------------------------------------------------------------------------------
std::vector<u8> GenerateBeaconFrame(const NetworkInfo& network_info,
                                    const std::vector<NodeInfo>& nodes);
/// Encrypts or decrypts (CTR is symmetric) the node list blob of a beacon, in place.
void DecryptBeacon(const NetworkInfo& network_info, u8* buffer, std::size_t size);

/// The vendor tags of a beacon body that the engine reads. Every pointer is at the tag's OUI (3 bytes,
/// then the tag type) and points into the body; null when the tag is missing.
struct BeaconTags {
    const u8* network = nullptr; ///< type 21: NetworkInfo from the OUI on (wlancommid BE32 at +4, id8 at +8)
    u32 network_size = 0;
    const u8* nodes[2] = {}; ///< types 24 and 25: the encrypted node list, in two parts
    u32 nodes_size[2] = {};
};
BeaconTags FindBeaconTags(const std::vector<u8>& body);
/// The node ids a beacon announces, as a mask (bit n-1 = node id n), read from its encrypted node list
/// the way a game does after a scan. False if the list is missing or its MD5 does not match. Azahar's
/// beacons leave the bitmask of the list empty, so the ids themselves are what counts.
bool GetBeaconNodeMask(const BeaconTags& tags, const Mac& host, u16* mask);
/// The application data a beacon carries (the bytes its host gave to SetApplicationData), read from the
/// NetworkInfo tag: `*data` points into the tag, `*size` is the count the tag's size byte gives. False if
/// the tag is missing or too short for what it says. This is what a client's GetApplicationData returns
/// (the firmware loads it from the host's current beacon; the 0x3C bytes of the old Connect carry none).
bool GetBeaconApplicationData(const BeaconTags& tags, const u8** data, u32* size);

// ---- Authentication / association ---------------------------------------------------------
enum class AuthSeq : u16 { Seq1 = 1, Seq2 = 2 };

std::vector<u8> GenerateAuthenticationFrame(AuthSeq seq);
bool GetAuthenticationSeqNumber(const std::vector<u8>& body, AuthSeq* out);
std::vector<u8> GenerateAssocResponseFrame(u16 status, u16 association_id, u32 network_id);
bool GetAssociationResult(const std::vector<u8>& body, u16* status, u16* association_id);

// ---- Data / EAPoL -------------------------------------------------------------------------
struct SecureDataHeader {
    u16 protocol_size = 0;
    u16 securedata_size = 0;
    u8 is_management = 0;
    u8 data_channel = 0;
    u16 sequence_number = 0;
    u16 dest_node_id = 0;
    u16 src_node_id = 0;

    u32 ActualDataSize() const {
        return protocol_size >= kSecureDataHeaderSize ? protocol_size - kSecureDataHeaderSize : 0;
    }
};

std::vector<u8> GenerateDataPayload(const u8* data, std::size_t size, u8 channel, u16 dest_node,
                                    u16 src_node, u16 sequence_number);
/// Parses the SecureData header of a data frame. Returns false if the frame is too short.
bool ParseSecureDataHeader(const std::vector<u8>& frame, SecureDataHeader* out);
/// Returns false if the frame is too short or has an unknown EtherType.
bool GetFrameEtherType(const std::vector<u8>& frame, EtherType* out);
bool GetEapolFrameType(const std::vector<u8>& frame, u16* out);

std::vector<u8> GenerateEapolStartFrame(u16 association_id, ConnectionType type,
                                        const NodeInfo& node);
struct EapolStart {
    u16 association_id = 0;
    u8 connection_type = 0;
    NodeInfo node;
};
bool ParseEapolStartFrame(const std::vector<u8>& frame, EapolStart* out);

std::vector<u8> GenerateEapolLogoffFrame(u16 network_node_id, const std::vector<NodeInfo>& nodes,
                                         u8 max_nodes, u8 total_nodes);
struct EapolLogoff {
    u16 assigned_node_id = 0;
    u8 connected_nodes = 0;
    u8 max_nodes = 0;
    NodeInfo nodes[kUdsMaxNodes];
};
bool ParseEapolLogoffFrame(const std::vector<u8>& frame, EapolLogoff* out);

// ---- NodeMap (Azahar-private packet, not 802.11) -----------------------------------------
struct NodeMapEntry {
    Mac mac;
    u16 node_id;
};
/// Layout: u64 LE count, then count * {mac[6], u16 LE node_id}. The count is a host `size_t`
/// in Azahar, i.e. 8 bytes on every platform Azahar runs on.
std::vector<u8> GenerateNodeMapPacket(const std::vector<NodeMapEntry>& entries);
bool ParseNodeMapPacket(const std::vector<u8>& data, std::vector<NodeMapEntry>* out);

} // namespace azl
