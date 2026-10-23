#include "azl/uds_frames.hpp"

#include <algorithm>

#include "azl/crypto.hpp"

namespace azl {

namespace {

constexpr u64 kDefaultNetworkUptime = 900000000; // 15 minutes in microseconds
// Values taken from a packet capture of an o3DS XL hosting a Super Smash Bros. 4 lobby.
constexpr u16 kDefaultExtraCapabilities = 0x0431;
constexpr u8 kUdsBeaconSsidSize = 8;
constexpr std::size_t kEncryptedDataSizeCutoff = 0xFA;

constexpr u8 kTagSsid = 0;
constexpr u8 kTagVendorSpecific = 221;

constexpr u8 kNintendoTagDummy = 20;
constexpr u8 kNintendoTagNetworkInfo = 21;
constexpr u8 kNintendoTagEncryptedData0 = 24;
constexpr u8 kNintendoTagEncryptedData1 = 25;

constexpr std::size_t kBeaconFrameHeaderSize = 12;
constexpr std::size_t kBeaconNodeInfoSize = 0x1E;
constexpr std::size_t kBeaconDataHeaderSize = 0x12; // md5[16] + bitmask(be16)
constexpr std::size_t kNetworkInfoTagSize = 54;
constexpr std::size_t kEncryptedTagHeaderSize = 6;

// The beacon key is all zeroes in Azahar (the real one is only needed to talk to a real 3DS).
constexpr u8 kNwmBeaconKey[16] = {};

void Append(std::vector<u8>& dst, const std::vector<u8>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

// ---- Beacon helpers ------------------------------------------------------------------------

std::vector<u8> GenerateFixedParameters() {
    std::vector<u8> buffer(kBeaconFrameHeaderSize);
    StoreLE64(buffer.data(), kDefaultNetworkUptime);
    StoreLE16(buffer.data() + 8, kDefaultBeaconInterval);
    StoreLE16(buffer.data() + 10, kDefaultExtraCapabilities);
    return buffer;
}

std::vector<u8> GenerateSsidTag() {
    std::vector<u8> buffer(2 + kUdsBeaconSsidSize, 0);
    buffer[0] = kTagSsid;
    buffer[1] = kUdsBeaconSsidSize;
    return buffer;
}

std::vector<u8> GenerateNintendoDummyTag() {
    // Values taken from the same capture as above.
    return {kTagVendorSpecific, 7, kNintendoOui[0], kNintendoOui[1], kNintendoOui[2],
            kNintendoTagDummy,  0x0A, 0x00, 0x00};
}

std::vector<u8> GenerateNintendoNetworkInfoTag(const NetworkInfo& ni) {
    const std::size_t app_size = std::min<std::size_t>(ni.application_data_size, kApplicationDataSize);
    std::vector<u8> buffer(kNetworkInfoTagSize + app_size, 0);
    buffer[0] = kTagVendorSpecific;
    buffer[1] = static_cast<u8>(kNetworkInfoTagSize - 2 + app_size);
    // The tag carries the network info starting at the OUI (offset 0xC), 0x1F bytes.
    std::memcpy(buffer.data() + 2, reinterpret_cast<const u8*>(&ni) + offsetof(NetworkInfo, oui_value),
                0x1F);
    // sha_hash[0x14] at 0x21 stays zero while hashing.
    buffer[53] = static_cast<u8>(app_size);
    std::memcpy(buffer.data() + kNetworkInfoTagSize, ni.application_data, app_size);

    u8 hash[20];
    Sha1(buffer.data() + 2, buffer.size() - 2, hash);
    std::memcpy(buffer.data() + 0x21, hash, sizeof(hash));
    return buffer;
}

void GetBeaconCryptoCtr(const NetworkInfo& ni, u8 ctr[16]) {
    std::memset(ctr, 0, 16);
    std::memcpy(ctr, ni.host_mac_address.data(), 6);
    StoreLE32(ctr + 6, ni.wlan_comm_id());
    ctr[10] = ni.id;
    // ctr[11] is padding
    StoreLE32(ctr + 12, ni.network_id());
}

std::vector<u8> GenerateEncryptedData(const NetworkInfo& ni, const std::vector<NodeInfo>& nodes) {
    std::vector<u8> buffer(kBeaconDataHeaderSize, 0);
    for (const NodeInfo& node : nodes) {
        u8 info[kBeaconNodeInfoSize];
        StoreBE64(info, node.friend_code_seed);
        for (int i = 0; i < 10; ++i) {
            StoreBE16(info + 8 + i * 2, node.username[i]);
        }
        StoreBE16(info + 0x1C, node.network_node_id);
        buffer.insert(buffer.end(), info, info + sizeof(info));
    }

    // MD5 over everything after the hash field (bitmask + nodes).
    u8 hash[16];
    Md5(buffer.data() + 16, buffer.size() - 16, hash);
    std::memcpy(buffer.data(), hash, sizeof(hash));

    u8 ctr[16];
    GetBeaconCryptoCtr(ni, ctr);
    Aes128CtrXor(kNwmBeaconKey, ctr, buffer.data(), buffer.size());
    return buffer;
}

std::vector<u8> GenerateEncryptedDataTag(u8 oui_type, const std::vector<u8>& encrypted,
                                         std::size_t offset, std::size_t payload) {
    std::vector<u8> buffer(kEncryptedTagHeaderSize + payload);
    buffer[0] = kTagVendorSpecific;
    buffer[1] = static_cast<u8>(kEncryptedTagHeaderSize - 2 + payload);
    buffer[2] = kNintendoOui[0];
    buffer[3] = kNintendoOui[1];
    buffer[4] = kNintendoOui[2];
    buffer[5] = oui_type;
    std::memcpy(buffer.data() + kEncryptedTagHeaderSize, encrypted.data() + offset, payload);
    return buffer;
}

// ---- LLC / SecureData helpers --------------------------------------------------------------

/// Writes the 8-byte LLC/SNAP header into `header` (which must be zeroed).
static void StoreLlcHeader(u8* header, EtherType protocol) {
    header[0] = 0xAA; // dsap: SNAP extension used
    header[1] = 0xAA; // ssap
    header[2] = 0x03; // control: unnumbered information
    // header[3..5] is the OUI, always zero
    StoreBE16(header + 6, static_cast<u16>(protocol));
}

std::vector<u8> GenerateLlcHeader(EtherType protocol) {
    std::vector<u8> header(kLlcHeaderSize, 0);
    StoreLlcHeader(header.data(), protocol);
    return header;
}

void StoreEapolNodeInfo(u8* out, const NodeInfo& node) {
    std::memset(out, 0, kEapolNodeInfoSize);
    StoreBE64(out, node.friend_code_seed);
    for (int i = 0; i < 10; ++i) {
        StoreBE16(out + 8 + i * 2, node.username[i]);
    }
    StoreBE16(out + 0x20, node.network_node_id);
}

NodeInfo LoadEapolNodeInfo(const u8* in) {
    NodeInfo node;
    node.friend_code_seed = LoadBE64(in);
    for (int i = 0; i < 10; ++i) {
        node.username[i] = LoadBE16(in + 8 + i * 2);
    }
    node.network_node_id = LoadBE16(in + 0x20);
    return node;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Beacon
// ---------------------------------------------------------------------------------------------

std::vector<u8> GenerateBeaconFrame(const NetworkInfo& ni, const std::vector<NodeInfo>& nodes) {
    std::vector<u8> buffer = GenerateFixedParameters();
    Append(buffer, GenerateSsidTag());
    Append(buffer, GenerateNintendoDummyTag());
    Append(buffer, GenerateNintendoNetworkInfoTag(ni));

    const std::size_t total = nodes.size() * kBeaconNodeInfoSize + kBeaconDataHeaderSize;
    const std::vector<u8> encrypted = GenerateEncryptedData(ni, nodes);

    const std::size_t first = std::min(kEncryptedDataSizeCutoff, total);
    Append(buffer, GenerateEncryptedDataTag(kNintendoTagEncryptedData0, encrypted, 0, first));
    if (total > kEncryptedDataSizeCutoff) {
        const std::size_t second = total - kEncryptedDataSizeCutoff;
        // A tag length must fit in a byte.
        if (kEncryptedTagHeaderSize - 2 + second <= 255) {
            Append(buffer, GenerateEncryptedDataTag(kNintendoTagEncryptedData1, encrypted,
                                                    kEncryptedDataSizeCutoff, second));
        }
    }
    return buffer;
}

void DecryptBeacon(const NetworkInfo& ni, u8* buffer, std::size_t size) {
    u8 ctr[16];
    GetBeaconCryptoCtr(ni, ctr);
    Aes128CtrXor(kNwmBeaconKey, ctr, buffer, size);
}

BeaconTags FindBeaconTags(const std::vector<u8>& body) {
    BeaconTags tags;
    for (std::size_t pos = kBeaconFrameHeaderSize; pos + 2 <= body.size();) {
        const u8 id = body[pos], len = body[pos + 1];
        pos += 2;
        if (pos + len > body.size()) {
            break;
        }
        if (id == kTagVendorSpecific && len >= 4 && std::memcmp(&body[pos], kNintendoOui, 3) == 0) {
            const u8 type = body[pos + 3];
            if (type == kNintendoTagNetworkInfo) {
                tags.network = &body[pos];
                tags.network_size = len;
            } else if (type == kNintendoTagEncryptedData0 || type == kNintendoTagEncryptedData1) {
                const int part = type == kNintendoTagEncryptedData0 ? 0 : 1;
                tags.nodes[part] = &body[pos];
                tags.nodes_size[part] = len;
            }
        }
        pos += len;
    }
    return tags;
}

bool GetBeaconNodeMask(const BeaconTags& tags, const Mac& host, u16* mask) {
    constexpr std::size_t kNetworkPrefix = 0x1F; // the NetworkInfo bytes the tag carries (from the OUI)
    constexpr std::size_t kTagHeader = 4;        // OUI + type
    if (!tags.network || tags.network_size < kNetworkPrefix || !tags.nodes[0]) {
        return false;
    }
    NetworkInfo ni;
    std::memset(&ni, 0, sizeof(ni));
    std::memcpy(ni.oui_value, tags.network, kNetworkPrefix);
    ni.host_mac_address = host;

    u8 blob[kBeaconDataHeaderSize + kUdsMaxNodes * kBeaconNodeInfoSize];
    std::size_t size = 0;
    for (int part = 0; part < 2; ++part) {
        if (!tags.nodes[part]) {
            continue;
        }
        if (tags.nodes_size[part] < kTagHeader || size + tags.nodes_size[part] - kTagHeader > sizeof(blob)) {
            return false;
        }
        std::memcpy(blob + size, tags.nodes[part] + kTagHeader, tags.nodes_size[part] - kTagHeader);
        size += tags.nodes_size[part] - kTagHeader;
    }
    if (size < kBeaconDataHeaderSize) {
        return false;
    }
    DecryptBeacon(ni, blob, size);
    u8 md5[16];
    Md5(blob + 16, size - 16, md5);
    if (std::memcmp(md5, blob, sizeof(md5)) != 0) {
        return false;
    }
    *mask = 0;
    const std::size_t slots = std::min<std::size_t>(ni.max_nodes, kUdsMaxNodes);
    for (std::size_t i = 0; i < slots && kBeaconDataHeaderSize + (i + 1) * kBeaconNodeInfoSize <= size; ++i) {
        const u16 id = LoadBE16(blob + kBeaconDataHeaderSize + i * kBeaconNodeInfoSize + 0x1C);
        if (id >= 1 && id <= kUdsMaxNodes) {
            *mask |= static_cast<u16>(1u << (id - 1));
        }
    }
    return true;
}

bool GetBeaconApplicationData(const BeaconTags& tags, const u8** data, u32* size) {
    // The tag payload (from the OUI on) is kNetworkInfoTagSize - 2 bytes ahead of the application data, the
    // last of them the size byte (GenerateNintendoNetworkInfoTag writes both).
    constexpr std::size_t kBeforeData = kNetworkInfoTagSize - 2;
    if (!tags.network || tags.network_size < kBeforeData) {
        return false;
    }
    const std::size_t count = tags.network[kBeforeData - 1];
    if (count > kApplicationDataSize || tags.network_size < kBeforeData + count) {
        return false;
    }
    *data = tags.network + kBeforeData;
    *size = static_cast<u32>(count);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Authentication / association
// ---------------------------------------------------------------------------------------------

std::vector<u8> GenerateAuthenticationFrame(AuthSeq seq) {
    std::vector<u8> data(6, 0);
    StoreLE16(data.data() + 0, 0); // OpenSystem
    StoreLE16(data.data() + 2, static_cast<u16>(seq));
    StoreLE16(data.data() + 4, 0); // Successful
    return data;
}

bool GetAuthenticationSeqNumber(const std::vector<u8>& body, AuthSeq* out) {
    if (body.size() < 6) {
        return false;
    }
    *out = static_cast<AuthSeq>(LoadLE16(body.data() + 2));
    return true;
}

std::vector<u8> GenerateAssocResponseFrame(u16 status, u16 association_id, u32 network_id) {
    std::vector<u8> data(6, 0);
    StoreLE16(data.data() + 0, kDefaultExtraCapabilities);
    StoreLE16(data.data() + 2, status);
    constexpr u16 kAssociationIdMagic = 0xC000;
    StoreLE16(data.data() + 4, static_cast<u16>(association_id | kAssociationIdMagic));

    // SSID tag: the network id as 8 upper-case hex characters.
    data.push_back(kTagSsid);
    data.push_back(8);
    static const char kHex[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4) {
        data.push_back(static_cast<u8>(kHex[(network_id >> shift) & 0xF]));
    }
    return data;
}

bool GetAssociationResult(const std::vector<u8>& body, u16* status, u16* association_id) {
    if (body.size() < 6) {
        return false;
    }
    *status = LoadLE16(body.data() + 2);
    *association_id = LoadLE16(body.data() + 4) & 0x3FFF;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Data / EAPoL
// ---------------------------------------------------------------------------------------------

std::vector<u8> GenerateDataPayload(const u8* data, std::size_t size, u8 channel, u16 dest_node,
                                    u16 src_node, u16 sequence_number) {
    // One allocation for the whole frame (every game frame goes through here: the header, then the data
    // appended, used to make the vector grow twice).
    std::vector<u8> buffer;
    buffer.reserve(kLlcHeaderSize + kSecureDataHeaderSize + size);
    buffer.resize(kLlcHeaderSize + kSecureDataHeaderSize, 0);
    StoreLlcHeader(buffer.data(), EtherType::SecureData);
    const std::size_t header_at = kLlcHeaderSize;
    u8* h = buffer.data() + header_at;
    StoreBE16(h + 0, static_cast<u16>(size + kSecureDataHeaderSize)); // protocol_size
    // h[2..3] padding
    // Includes everything except the first 4 bytes of the structure.
    StoreBE16(h + 4, static_cast<u16>(size + kSecureDataHeaderSize - 4)); // securedata_size
    h[6] = 0;                                                             // is_management
    h[7] = channel;
    StoreBE16(h + 8, sequence_number);
    StoreBE16(h + 10, dest_node);
    StoreBE16(h + 12, src_node);
    buffer.insert(buffer.end(), data, data + size);
    return buffer;
}

bool ParseSecureDataHeader(const std::vector<u8>& frame, SecureDataHeader* out) {
    if (frame.size() < kLlcHeaderSize + kSecureDataHeaderSize) {
        return false;
    }
    const u8* h = frame.data() + kLlcHeaderSize;
    out->protocol_size = LoadBE16(h + 0);
    out->securedata_size = LoadBE16(h + 4);
    out->is_management = h[6];
    out->data_channel = h[7];
    out->sequence_number = LoadBE16(h + 8);
    out->dest_node_id = LoadBE16(h + 10);
    out->src_node_id = LoadBE16(h + 12);
    return true;
}

bool GetFrameEtherType(const std::vector<u8>& frame, EtherType* out) {
    if (frame.size() < kLlcHeaderSize) {
        return false;
    }
    const u16 value = LoadBE16(frame.data() + 6);
    if (value != static_cast<u16>(EtherType::SecureData) &&
        value != static_cast<u16>(EtherType::EAPoL)) {
        return false;
    }
    *out = static_cast<EtherType>(value);
    return true;
}

bool GetEapolFrameType(const std::vector<u8>& frame, u16* out) {
    if (frame.size() < kLlcHeaderSize + 2) {
        return false;
    }
    *out = LoadBE16(frame.data() + kLlcHeaderSize);
    return true;
}

std::vector<u8> GenerateEapolStartFrame(u16 association_id, ConnectionType type,
                                        const NodeInfo& node) {
    std::vector<u8> buffer = GenerateLlcHeader(EtherType::EAPoL);
    const std::size_t at = buffer.size();
    buffer.resize(at + kEapolStartSize, 0);
    u8* p = buffer.data() + at;
    StoreBE16(p + 0, kEapolStartMagic);
    StoreBE16(p + 2, association_id);
    p[4] = static_cast<u8>(type);
    // p[5..7] padding
    StoreEapolNodeInfo(p + 8, node);
    // Like Azahar, the node's network_node_id is left uninitialised (zero) in the start packet.
    StoreBE16(p + 8 + 0x20, 0);
    return buffer;
}

bool ParseEapolStartFrame(const std::vector<u8>& frame, EapolStart* out) {
    if (frame.size() < kLlcHeaderSize + kEapolStartSize) {
        return false;
    }
    const u8* p = frame.data() + kLlcHeaderSize;
    out->association_id = LoadBE16(p + 2);
    out->connection_type = p[4];
    out->node = LoadEapolNodeInfo(p + 8);
    return true;
}

std::vector<u8> GenerateEapolLogoffFrame(u16 network_node_id, const std::vector<NodeInfo>& nodes,
                                         u8 max_nodes, u8 total_nodes) {
    std::vector<u8> buffer = GenerateLlcHeader(EtherType::EAPoL);
    const std::size_t at = buffer.size();
    buffer.resize(at + kEapolLogoffSize, 0);
    u8* p = buffer.data() + at;
    StoreBE16(p + 0, kEapolLogoffMagic);
    StoreBE16(p + 4, network_node_id); // assigned_node_id
    // p[6..11] client mac stays zero (Azahar never fills it), p[12..17] padding
    p[0x12] = total_nodes;
    p[0x13] = max_nodes;
    const std::size_t count = std::min<std::size_t>(std::min<std::size_t>(max_nodes, kUdsMaxNodes),
                                                    nodes.size());
    for (std::size_t i = 0; i < count; ++i) {
        StoreEapolNodeInfo(p + 0x18 + i * kEapolNodeInfoSize, nodes[i]);
    }
    return buffer;
}

bool ParseEapolLogoffFrame(const std::vector<u8>& frame, EapolLogoff* out) {
    if (frame.size() < kLlcHeaderSize + kEapolLogoffSize) {
        return false;
    }
    const u8* p = frame.data() + kLlcHeaderSize;
    out->assigned_node_id = LoadBE16(p + 4);
    out->connected_nodes = p[0x12];
    out->max_nodes = p[0x13];
    for (std::size_t i = 0; i < kUdsMaxNodes; ++i) {
        out->nodes[i] = LoadEapolNodeInfo(p + 0x18 + i * kEapolNodeInfoSize);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// NodeMap
// ---------------------------------------------------------------------------------------------

std::vector<u8> GenerateNodeMapPacket(const std::vector<NodeMapEntry>& entries) {
    std::vector<u8> data(8 + entries.size() * 8, 0);
    StoreLE64(data.data(), entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        u8* p = data.data() + 8 + i * 8;
        std::memcpy(p, entries[i].mac.data(), 6);
        StoreLE16(p + 6, entries[i].node_id);
    }
    return data;
}

bool ParseNodeMapPacket(const std::vector<u8>& data, std::vector<NodeMapEntry>* out) {
    out->clear();
    if (data.size() < 8) {
        return false;
    }
    const u64 count = LoadLE64(data.data());
    if (count > (data.size() - 8) / 8) {
        return false;
    }
    for (u64 i = 0; i < count; ++i) {
        const u8* p = data.data() + 8 + i * 8;
        NodeMapEntry entry;
        std::memcpy(entry.mac.data(), p, 6);
        entry.node_id = LoadLE16(p + 6);
        out->push_back(entry);
    }
    return true;
}

} // namespace azl
