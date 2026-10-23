// A model of an AZAHAR emulator hosting a UDS network, as the room sees it: the host side of Azahar's
// nwm_uds.cpp, frozen. Our own UdsEngine hosts the same way today, but it is going to change (it stops
// re-broadcasting client-to-client frames that were sent to the broadcast MAC, answers like a real
// nwm, ...) while every Azahar player in a room keeps behaving exactly like this. Tests that need "the
// other side is Azahar" (the FFE replay, the result-set closure) use this model instead of an engine.
//
// What it does, in Azahar's own terms:
//   Authentication SEQ1   -> Authentication SEQ2 + AssociationResponse(status 0), unless the MAC is
//                            known already or the network is full (Azahar sends no reject frame)
//   EAPoL-Start           -> node id = first free slot, node map + EAPoL-Logoff BROADCAST (it ASSERTs
//                            when the network is full: reported to the rule checker)
//   Deauthentication      -> the node is dropped (no frame, no notification: Azahar never tells the
//                            other clients)
//   SecureData            -> for the host (node 1 or 0xFFFF): kept in `inbox`; for any other node:
//                            re-broadcast to the whole room, whatever the 802.3 destination was
//   beacons               -> only when the test calls SendBeacon() (the real thing does it every 102 ms)
#pragma once

#include <algorithm>
#include <mutex>

#include "mock_room.hpp"

/// Rebuilds the NetworkInfo a game builds from the first entry of a scan reply (what libctru's
/// udsScanBeacons does: the network tag verbatim, the host MAC and channel from the entry header).
inline bool FirstScannedNetwork(const std::vector<u8>& scan, NetworkInfo* out) {
    if (scan.size() < kBeaconDataReplyHeaderSize + kBeaconEntryHeaderSize ||
        LoadLE32(scan.data() + 8) == 0) {
        return false;
    }
    const u8* entry = scan.data() + kBeaconDataReplyHeaderSize;
    const u32 entry_size = LoadLE32(entry);
    if (entry_size < kBeaconEntryHeaderSize || kBeaconDataReplyHeaderSize + entry_size > scan.size()) {
        return false;
    }
    const u8* beacon = entry + kBeaconEntryHeaderSize;
    const std::size_t beacon_size = entry_size - kBeaconEntryHeaderSize;
    const u8* net_tag = nullptr;
    for (std::size_t pos = 12; pos + 2 <= beacon_size;) {
        const u8 id = beacon[pos], len = beacon[pos + 1];
        pos += 2;
        if (id == 0xdd && pos + 4 <= beacon_size && std::memcmp(beacon + pos, kNintendoOui, 3) == 0 &&
            beacon[pos + 3] == 21) {
            net_tag = beacon + pos;
        }
        pos += len;
    }
    if (!net_tag) {
        return false;
    }
    std::memset(out, 0, sizeof(*out));
    std::memcpy(out->oui_value, net_tag, 0x1F);
    out->application_data_size = net_tag[0x33];
    std::memcpy(out->application_data, net_tag + 0x34, std::min<std::size_t>(out->application_data_size, kApplicationDataSize));
    out->channel = entry[5];
    std::memcpy(out->host_mac_address.data(), entry + 8, 6);
    return true;
}

class AzaharHost {
public:
    struct Frame {
        u16 src;
        u16 dest;
        u8 channel;
        std::vector<u8> payload;
    };

    /// `net` supplies max_nodes, wlancommid, id, attributes, network id and the application data;
    /// everything else is filled in like BeginHostingNetwork does.
    AzaharHost(MockRoom& room, u8 mac_last, const NetworkInfo& net, const NodeInfo& self) : room_(room) {
        mac_ = Mac{0x00, 0x1F, 0x32, 0x00, 0x00, mac_last};
        net_ = net;
        std::memcpy(net_.oui_value, kNintendoOui, sizeof(kNintendoOui));
        net_.oui_type = 21;
        net_.host_mac_address = mac_;
        if (net_.channel == 0) {
            net_.channel = kDefaultNetworkChannel;
        }
        net_.total_nodes = 1;
        nodes_.assign(net_.max_nodes, NodeInfo{});
        nodes_[0] = self;
        nodes_[0].network_node_id = 1;
        index_ = room_.AddMember(mac_, [this](const WifiPacket& p) { OnPacket(p); });
    }
    ~AzaharHost() {
        room_.RemoveMember(index_);
    }

    const Mac& mac() const {
        return mac_;
    }
    /// The NetworkInfo a scan would report (host MAC and channel included).
    NetworkInfo network() const {
        std::lock_guard<std::mutex> l(m_);
        return net_;
    }
    u8 total() const {
        std::lock_guard<std::mutex> l(m_);
        return net_.total_nodes;
    }
    u32 admissions() const {
        std::lock_guard<std::mutex> l(m_);
        return admissions_;
    }
    u32 departures() const {
        std::lock_guard<std::mutex> l(m_);
        return departures_;
    }
    /// Frames the host re-broadcast for other nodes.
    u32 forwarded() const {
        std::lock_guard<std::mutex> l(m_);
        return forwarded_;
    }
    /// Frames addressed to the host itself (node 1 or the broadcast node), oldest first.
    std::vector<Frame> TakeInbox() {
        std::lock_guard<std::mutex> l(m_);
        std::vector<Frame> out;
        out.swap(inbox_);
        return out;
    }

    void SendBeacon() {
        std::lock_guard<std::mutex> l(m_);
        WifiPacket p;
        p.type = WifiPacket::Type::Beacon;
        p.data = GenerateBeaconFrame(net_, nodes_);
        p.destination_address = kBroadcastMac;
        p.channel = net_.channel;
        Send(p);
    }

    /// The host's own SendTo (flags bit 1 = broadcast MAC, like the FFE traffic).
    bool SendData(u16 dest, u8 channel, u8 flags, const std::vector<u8>& payload) {
        std::lock_guard<std::mutex> l(m_);
        WifiPacket p;
        p.type = WifiPacket::Type::Data;
        p.channel = net_.channel;
        p.data = GenerateDataPayload(payload.data(), payload.size(), channel, dest, 1, 0);
        if ((flags & 0x2) || dest == kBroadcastNetworkNodeId) {
            p.destination_address = kBroadcastMac;
        } else {
            bool found = false;
            for (const auto& node : node_map_) {
                if (node.second.connected && node.second.node_id == dest) {
                    p.destination_address = node.first;
                    found = true;
                }
            }
            if (!found) {
                return false;
            }
        }
        Send(p);
        return true;
    }

private:
    struct Node {
        bool connected = false;
        bool spec = false;
        u16 node_id = 0;
    };

    void Send(WifiPacket& p) { // m_ held
        p.transmitter_address = mac_;
        g_rules.Inspect(p);
        room_.Send(index_, p);
    }

    u16 NextNodeId() const {
        for (u16 i = 0; i < net_.max_nodes && i < kUdsMaxNodes; ++i) {
            if ((bitmask_ & (1u << i)) == 0) {
                return static_cast<u16>(i + 1);
            }
        }
        return 0;
    }

    void BroadcastNodeMap() {
        std::vector<NodeMapEntry> entries;
        for (const auto& node : node_map_) {
            if (node.second.connected && !node.second.spec) {
                entries.push_back({node.first, node.second.node_id});
            }
        }
        WifiPacket p;
        p.channel = net_.channel;
        p.type = WifiPacket::Type::NodeMap;
        p.destination_address = kBroadcastMac;
        p.data = GenerateNodeMapPacket(entries);
        Send(p);
    }

    void OnPacket(const WifiPacket& p) {
        std::lock_guard<std::mutex> l(m_);
        switch (p.type) {
        case WifiPacket::Type::Authentication: {
            AuthSeq seq;
            if (!GetAuthenticationSeqNumber(p.data, &seq) || seq != AuthSeq::Seq1) {
                return;
            }
            if (node_map_.count(p.transmitter_address) || net_.max_nodes == net_.total_nodes) {
                return;
            }
            WifiPacket auth;
            auth.channel = net_.channel;
            auth.data = GenerateAuthenticationFrame(AuthSeq::Seq2);
            auth.destination_address = p.transmitter_address;
            auth.type = WifiPacket::Type::Authentication;
            node_map_[p.transmitter_address].connected = false;
            Send(auth);
            WifiPacket assoc;
            assoc.channel = net_.channel;
            assoc.data = GenerateAssocResponseFrame(0, 1, net_.network_id());
            assoc.destination_address = p.transmitter_address;
            assoc.type = WifiPacket::Type::AssociationResponse;
            Send(assoc);
            break;
        }
        case WifiPacket::Type::Data: {
            EtherType type;
            if (!GetFrameEtherType(p.data, &type)) {
                return;
            }
            if (type == EtherType::EAPoL) {
                OnEapol(p);
            } else {
                OnSecureData(p);
            }
            break;
        }
        case WifiPacket::Type::Deauthentication: {
            auto it = node_map_.find(p.transmitter_address);
            if (it == node_map_.end()) {
                return;
            }
            const Node node = it->second;
            node_map_.erase(it);
            if (!node.connected) {
                return;
            }
            if (!node.spec && node.node_id >= 1 && node.node_id <= kUdsMaxNodes) {
                bitmask_ &= static_cast<u16>(~(1u << (node.node_id - 1)));
                --net_.total_nodes;
                nodes_[node.node_id - 1] = NodeInfo{};
            }
            ++departures_;
            break;
        }
        default:
            break;
        }
    }

    void OnEapol(const WifiPacket& p) {
        u16 magic = 0;
        if (!GetEapolFrameType(p.data, &magic) || magic != kEapolStartMagic) {
            return;
        }
        auto it = node_map_.find(p.transmitter_address);
        if (it == node_map_.end() || it->second.connected) {
            return; // never authenticated, or admitted already
        }
        if (net_.max_nodes == net_.total_nodes) {
            g_rules.Fail("azahar-assert-224", "EAPoL-Start from " + Hex(p.transmitter_address.data(), 6) +
                                                  " reached a full host (Azahar ASSERTs: max_nodes != total_nodes)");
            return;
        }
        EapolStart start;
        if (!ParseEapolStartFrame(p.data, &start)) {
            return;
        }
        NodeInfo node = start.node;
        if (start.connection_type == static_cast<u8>(ConnectionType::Client)) {
            const u16 id = NextNodeId();
            if (id == 0 || id > nodes_.size()) {
                return;
            }
            node.network_node_id = id;
            bitmask_ |= static_cast<u16>(1u << (id - 1));
            nodes_[id - 1] = node;
            ++net_.total_nodes;
            Node& entry = node_map_[p.transmitter_address];
            entry.node_id = id;
            entry.connected = true;
            entry.spec = false;
            BroadcastNodeMap();
        } else {
            Node& entry = node_map_[p.transmitter_address];
            entry.node_id = kNodeIdSpec;
            entry.connected = true;
            entry.spec = true;
        }
        WifiPacket logoff;
        logoff.channel = net_.channel;
        logoff.data = GenerateEapolLogoffFrame(node.network_node_id, nodes_, net_.max_nodes, net_.total_nodes);
        logoff.destination_address = kBroadcastMac;
        logoff.type = WifiPacket::Type::Data;
        Send(logoff);
        ++admissions_;
    }

    void OnSecureData(const WifiPacket& p) {
        SecureDataHeader h;
        if (!ParseSecureDataHeader(p.data, &h) || h.src_node_id == 1) {
            return;
        }
        if (h.dest_node_id != 1 && h.dest_node_id != kBroadcastNetworkNodeId) {
            // Azahar: a frame for another node is re-broadcast, whatever the 802.3 destination was.
            WifiPacket out = p;
            out.destination_address = kBroadcastMac;
            Send(out);
            ++forwarded_;
            return;
        }
        if (h.is_management) {
            return; // (the rule checker flags the sender)
        }
        const std::size_t at = kLlcHeaderSize + kSecureDataHeaderSize;
        std::vector<u8> payload;
        if (p.data.size() > at) {
            payload.assign(p.data.begin() + at, p.data.begin() + at + std::min<std::size_t>(h.ActualDataSize(), p.data.size() - at));
        }
        inbox_.push_back({h.src_node_id, h.dest_node_id, h.data_channel, std::move(payload)});
    }

    MockRoom& room_;
    mutable std::mutex m_;
    Mac mac_{};
    int index_ = -1;
    NetworkInfo net_{};
    std::vector<NodeInfo> nodes_;
    u16 bitmask_ = 1;
    std::map<Mac, Node> node_map_;
    std::vector<Frame> inbox_;
    u32 admissions_ = 0;
    u32 departures_ = 0;
    u32 forwarded_ = 0;
};

// A model of an AZAHAR emulator as a CLIENT of a UDS network, the other half of nwm_uds.cpp, frozen the same way (release
// 2126.1.2, the one the room server of the integration tests is built from). Our own engine joins a lobby the same way
// today, and the tests where the 3DS is the HOST need the players around it to be Azahar, whatever the engine becomes.
//
// What it does, in Azahar's own terms:
//   Connect              -> status Connecting + Authentication SEQ1 to the host's MAC (StartConnectionSequence)
//   AssociationResponse  -> in Connecting only: EAPoL-Start (association id, connection type, our NodeInfo) to the sender
//   EAPoL-Logoff         -> in Connecting: the host is the sender, our node id is the one in the frame, status
//                           ConnectedAsClient / ConnectedAsSpectator (the first Logoff that arrives, whoever it was for);
//                           when connected: the node list of the frame replaces ours
//   NodeMap              -> replaces the map MAC -> node id (this is how a client learns the other clients' MACs)
//   Deauthentication     -> ignored (a client "got deauthentication frame but we are not the host")
//   SecureData           -> not ours (source = our node id) dropped; for another node: dropped (an error when the 802.3
//                           destination was not the broadcast MAC); else queued on the channel's bind when the bind takes
//                           that source - no check of the transmitter, ever
//   SendTo               -> 802.3 destination: broadcast MAC when the flags have bit 1 or the node is 0xFFFF, the host's
//                           MAC for node 1, else the MAC the NodeMap gave for that node; sequence number always 0
//   Disconnect           -> status NotConnected, Deauthentication to the host, binds dropped
class AzaharClient {
public:
    using Frame = AzaharHost::Frame;

    AzaharClient(MockRoom& room, const Mac& mac, const NodeInfo& self) : room_(room), mac_(mac), self_(self) {
        index_ = room_.AddMember(mac_, [this](const WifiPacket& p) { OnPacket(p); });
    }
    AzaharClient(MockRoom& room, u8 mac_last, const NodeInfo& self)
        : AzaharClient(room, Mac{0x00, 0x1F, 0x32, 0x00, 0x00, mac_last}, self) {}
    ~AzaharClient() {
        room_.RemoveMember(index_);
    }

    const Mac& mac() const {
        return mac_;
    }
    /// ConnectToNetworkHLE: `net.host_mac_address` is the host to join (a scan fills the rest in a game).
    void Connect(const NetworkInfo& net, ConnectionType type = ConnectionType::Client) {
        WifiPacket auth;
        {
            std::lock_guard<std::mutex> l(m_);
            net_ = net;
            conn_type_ = type;
            status_ = NetworkStatus::Connecting;
            auth.channel = net_.channel ? net_.channel : kDefaultNetworkChannel;
            auth.data = GenerateAuthenticationFrame(AuthSeq::Seq1);
            auth.destination_address = net_.host_mac_address;
            auth.type = WifiPacket::Type::Authentication;
        }
        Send(auth);
    }
    /// BindHLE: frames of `channel` from `src_node` (0xFFFF = any) are kept for TakeInbox().
    void Bind(u8 channel, u16 src_node = kBroadcastNetworkNodeId) {
        std::lock_guard<std::mutex> l(m_);
        channels_[channel].bind_node = src_node;
    }
    /// SendToHLE. False for every case Azahar answers with an error (not connected, node 0, ourselves, no MAC known, too big).
    bool SendTo(u16 dest, u8 channel, u8 flags, const std::vector<u8>& payload) {
        WifiPacket p;
        {
            std::lock_guard<std::mutex> l(m_);
            if (status_ != NetworkStatus::ConnectedAsClient || dest == 0 || dest == node_id_ || payload.size() > 0x5C6) {
                return false;
            }
            Mac to;
            if ((flags & 0x2) || dest == kBroadcastNetworkNodeId) {
                to = kBroadcastMac;
            } else if (dest == kHostDestNodeId) {
                to = net_.host_mac_address;
            } else {
                bool found = false;
                for (const auto& node : node_map_) {
                    if (node.second.connected && node.second.node_id == dest) {
                        to = node.first;
                        found = true;
                    }
                }
                if (!found) {
                    return false;
                }
            }
            p.destination_address = to;
            p.channel = net_.channel ? net_.channel : kDefaultNetworkChannel;
            p.data = GenerateDataPayload(payload.data(), payload.size(), channel, dest, node_id_, 0);
            p.type = WifiPacket::Type::Data;
        }
        Send(p);
        return true;
    }
    /// DisconnectNetworkHLE (called as a client).
    void Disconnect() {
        WifiPacket deauth;
        {
            std::lock_guard<std::mutex> l(m_);
            status_ = NetworkStatus::NotConnected;
            node_map_.clear();
            channels_.clear();
            deauth.channel = net_.channel ? net_.channel : kDefaultNetworkChannel;
            deauth.destination_address = net_.host_mac_address;
            deauth.type = WifiPacket::Type::Deauthentication;
        }
        Send(deauth);
    }
    /// A Deauthentication to `host` that is not the one Disconnect() sends (the second copy a guest was seen to send).
    void SendDeauthTo(const Mac& host) {
        WifiPacket deauth;
        deauth.channel = kDefaultNetworkChannel;
        deauth.destination_address = host;
        deauth.type = WifiPacket::Type::Deauthentication;
        Send(deauth);
    }
    /// A data frame of ANOTHER lobby of the room whose members have this PC's MAC: the PC is a node of some other
    /// network (it took part in one, or hosts it), so its frames carry that network's node ids.
    void SendInOtherLobby(u16 src_node, u16 dest_node, u8 channel, const std::vector<u8>& payload, const Mac& to = kBroadcastMac) {
        WifiPacket p;
        p.type = WifiPacket::Type::Data;
        p.channel = kDefaultNetworkChannel;
        p.destination_address = to;
        p.data = GenerateDataPayload(payload.data(), payload.size(), channel, dest_node, src_node, 0);
        Send(p);
    }

    NetworkStatus status() const {
        std::lock_guard<std::mutex> l(m_);
        return status_;
    }
    bool connected() const {
        return status() == NetworkStatus::ConnectedAsClient;
    }
    u16 node_id() const {
        std::lock_guard<std::mutex> l(m_);
        return node_id_;
    }
    u8 total_nodes() const {
        std::lock_guard<std::mutex> l(m_);
        return total_;
    }
    /// Frames for us that arrived on a bound channel, oldest first.
    std::vector<Frame> TakeInbox() {
        std::lock_guard<std::mutex> l(m_);
        std::vector<Frame> out;
        out.swap(inbox_);
        return out;
    }
    /// Frames "addressed to others" that reached us with a unicast 802.3 destination (Azahar logs an error and drops them).
    u32 misrouted() const {
        std::lock_guard<std::mutex> l(m_);
        return misrouted_;
    }

private:
    struct Channel {
        u16 bind_node = kBroadcastNetworkNodeId;
    };
    struct Peer {
        bool connected = false;
        u16 node_id = 0;
    };

    void Send(WifiPacket& p) {
        p.transmitter_address = mac_;
        g_rules.Inspect(p);
        room_.Send(index_, p);
    }

    void OnPacket(const WifiPacket& p) {
        switch (p.type) {
        case WifiPacket::Type::AssociationResponse: {
            u16 status = 0, assoc = 0;
            WifiPacket start;
            {
                std::lock_guard<std::mutex> l(m_);
                if (!GetAssociationResult(p.data, &status, &assoc) || status != 0 || status_ != NetworkStatus::Connecting) {
                    return;
                }
                start.channel = net_.channel ? net_.channel : kDefaultNetworkChannel;
                start.data = GenerateEapolStartFrame(assoc, conn_type_, self_);
                start.destination_address = p.transmitter_address;
                start.type = WifiPacket::Type::Data;
            }
            Send(start);
            break;
        }
        case WifiPacket::Type::Data: {
            EtherType type;
            if (!GetFrameEtherType(p.data, &type)) {
                return;
            }
            std::lock_guard<std::mutex> l(m_);
            if (type == EtherType::EAPoL) {
                OnEapol(p);
            } else {
                OnSecureData(p);
            }
            break;
        }
        case WifiPacket::Type::NodeMap: {
            std::vector<NodeMapEntry> entries;
            std::lock_guard<std::mutex> l(m_);
            if (!ParseNodeMapPacket(p.data, &entries)) {
                return;
            }
            node_map_.clear();
            for (const NodeMapEntry& e : entries) {
                node_map_[e.mac].connected = true;
                node_map_[e.mac].node_id = e.node_id;
            }
            break;
        }
        default:
            break; // beacons are scans', Authentication is for hosts, a client ignores the host's Deauthentication
        }
    }

    void OnEapol(const WifiPacket& p) { // m_ held
        u16 magic = 0;
        if (!GetEapolFrameType(p.data, &magic) || magic != kEapolLogoffMagic) {
            return; // an EAPoL-Start is for hosts
        }
        EapolLogoff logoff;
        if (!ParseEapolLogoffFrame(p.data, &logoff)) {
            return;
        }
        if (status_ == NetworkStatus::Connecting) {
            net_.host_mac_address = p.transmitter_address;
            node_id_ = logoff.assigned_node_id;
            total_ = logoff.connected_nodes;
            status_ = conn_type_ == ConnectionType::Spectator ? NetworkStatus::ConnectedAsSpectator : NetworkStatus::ConnectedAsClient;
        } else if (status_ == NetworkStatus::ConnectedAsClient || status_ == NetworkStatus::ConnectedAsSpectator) {
            total_ = logoff.connected_nodes;
        }
    }

    void OnSecureData(const WifiPacket& p) { // m_ held
        SecureDataHeader h;
        if (!ParseSecureDataHeader(p.data, &h)) {
            return;
        }
        if (status_ != NetworkStatus::ConnectedAsClient && status_ != NetworkStatus::ConnectedAsSpectator) {
            return;
        }
        if (h.src_node_id == node_id_) {
            return; // our own
        }
        if (h.dest_node_id != node_id_ && h.dest_node_id != kBroadcastNetworkNodeId) {
            if (p.destination_address != kBroadcastMac) {
                ++misrouted_;
            }
            return;
        }
        if (h.is_management) {
            return; // (Azahar ASSERTs; the rule checker flags the sender)
        }
        const auto channel = channels_.find(h.data_channel);
        if (channel == channels_.end()) {
            return;
        }
        if (channel->second.bind_node != kBroadcastNetworkNodeId && channel->second.bind_node != h.src_node_id) {
            return;
        }
        const std::size_t at = kLlcHeaderSize + kSecureDataHeaderSize;
        std::vector<u8> payload;
        if (p.data.size() > at) {
            payload.assign(p.data.begin() + at, p.data.begin() + at + std::min<std::size_t>(h.ActualDataSize(), p.data.size() - at));
        }
        inbox_.push_back({h.src_node_id, h.dest_node_id, h.data_channel, std::move(payload)});
    }

    MockRoom& room_;
    mutable std::mutex m_;
    Mac mac_{};
    NodeInfo self_;
    int index_ = -1;
    NetworkInfo net_{};
    ConnectionType conn_type_ = ConnectionType::Client;
    NetworkStatus status_ = NetworkStatus::NotConnected;
    u16 node_id_ = 0;
    u8 total_ = 0;
    std::map<Mac, Peer> node_map_;
    std::map<u8, Channel> channels_;
    std::vector<Frame> inbox_;
    u32 misrouted_ = 0;
};
