// room_observe: passive field analyser. Joins an Azahar room and sends NOTHING except the join, then
// decodes every frame the room relays to it (beacons, node maps, EAPoL, broadcast data...) with
// AzaharLink's own parsers. A real-world check of those parsers against live Azahar traffic.
//
//   room_observe <host> [port] [seconds] [nickname]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "azl/crypto.hpp"
#include "azl/room_client.hpp"
#include "azl/uds_frames.hpp"

using namespace azl;

namespace {

std::string Hex(const u8* p, std::size_t n, std::size_t max = 16) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n && i < max; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    if (n > max) {
        s += "...";
    }
    return s;
}

std::string MacStr(const Mac& m) {
    char b[24];
    std::snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

std::string Name(const u16* units) {
    std::string s;
    for (int i = 0; i < 10 && units[i]; ++i) {
        s += units[i] < 0x80 ? static_cast<char>(units[i]) : '?';
    }
    return s;
}

struct Stats {
    u64 packets = 0, bytes = 0;
    std::size_t min_size = ~std::size_t(0), max_size = 0;
    void Add(std::size_t n) {
        ++packets;
        bytes += n;
        min_size = std::min(min_size, n);
        max_size = std::max(max_size, n);
    }
};

std::mutex g_mutex;
u64 g_start_ms = 0;
std::map<std::string, Stats> g_stats;
std::map<std::string, int> g_flow_samples; // limits repeated lines
int g_printed = 0;

const char* TypeName(WifiPacket::Type t) {
    switch (t) {
    case WifiPacket::Type::Beacon:
        return "Beacon";
    case WifiPacket::Type::Data:
        return "Data";
    case WifiPacket::Type::Authentication:
        return "Auth";
    case WifiPacket::Type::AssociationResponse:
        return "AssocResp";
    case WifiPacket::Type::Deauthentication:
        return "Deauth";
    case WifiPacket::Type::NodeMap:
        return "NodeMap";
    }
    return "?";
}

void Say(const char* tag, const std::string& line, int max_per_tag = 6) {
    int& n = g_flow_samples[tag];
    if (++n > max_per_tag) {
        return;
    }
    ++g_printed;
    std::printf("[%6llu ms] %-9s %s\n", static_cast<unsigned long long>(platform::NowMs() - g_start_ms),
                tag, line.c_str());
}

void DescribeBeacon(const WifiPacket& p) {
    const std::vector<u8>& b = p.data;
    if (b.size() < 12) {
        Say("Beacon", "short beacon");
        return;
    }
    const u8* net_tag = nullptr;
    std::size_t net_len = 0;
    std::vector<u8> enc;
    bool bad = false;
    for (std::size_t pos = 12; pos + 2 <= b.size();) {
        const u8 id = b[pos], len = b[pos + 1];
        pos += 2;
        if (pos + len > b.size()) {
            bad = true;
            break;
        }
        if (id == 0xdd && len >= 4 && std::memcmp(&b[pos], kNintendoOui, 3) == 0) {
            const u8 type = b[pos + 3];
            if (type == 21) {
                net_tag = &b[pos];
                net_len = len;
            } else if (type == 24 || type == 25) {
                enc.insert(enc.end(), &b[pos] + 4, &b[pos] + len);
            }
        }
        pos += len;
    }
    if (bad || !net_tag || net_len < 0x34) {
        Say("Beacon", "malformed beacon (" + std::to_string(b.size()) + " bytes)");
        return;
    }
    NetworkInfo ni;
    std::memset(&ni, 0, sizeof(ni));
    std::memcpy(ni.oui_value, net_tag, 0x1F);
    ni.host_mac_address = p.transmitter_address;
    ni.application_data_size = net_tag[0x33];
    std::memcpy(ni.application_data, net_tag + 0x34,
                std::min<std::size_t>(ni.application_data_size, kApplicationDataSize));
    DecryptBeacon(ni, enc.data(), enc.size());

    std::string nodes;
    for (u32 i = 0; i < ni.max_nodes && i < kUdsMaxNodes; ++i) {
        const std::size_t at = 0x12 + i * 0x1E;
        if (at + 0x1E > enc.size()) {
            break;
        }
        const u8* n = enc.data() + at;
        const u16 id = LoadBE16(n + 0x1C);
        if (!id) {
            continue;
        }
        u16 name[10];
        for (int k = 0; k < 10; ++k) {
            name[k] = LoadBE16(n + 8 + k * 2);
        }
        char one[96];
        std::snprintf(one, sizeof(one), " #%u:%s(seed %016llx)", id, Name(name).c_str(),
                      static_cast<unsigned long long>(LoadBE64(n)));
        nodes += one;
    }
    char line[512];
    std::snprintf(line, sizeof(line),
                  "from %s ch%u  wlancommid=%08X id=%u attr=%04X netid=%08X nodes %u/%u appdata=%u [%s]%s",
                  MacStr(p.transmitter_address).c_str(), p.channel, ni.wlan_comm_id(), ni.id,
                  LoadBE16(ni.attributes_be), ni.network_id(), ni.total_nodes, ni.max_nodes,
                  ni.application_data_size, Hex(ni.application_data, ni.application_data_size, 24).c_str(),
                  nodes.c_str());
    Say("Beacon", line, 3);
}

void OnPacket(const WifiPacket& p) {
    std::lock_guard<std::mutex> lock(g_mutex);
    char key[64];
    std::snprintf(key, sizeof(key), "%s%s", TypeName(p.type),
                  p.destination_address == kBroadcastMac ? " (bcast)" : " (unicast to me)");
    g_stats[key].Add(p.data.size());

    switch (p.type) {
    case WifiPacket::Type::Beacon:
        DescribeBeacon(p);
        break;
    case WifiPacket::Type::NodeMap: {
        std::vector<NodeMapEntry> entries;
        std::string s = "from " + MacStr(p.transmitter_address) + ":";
        if (ParseNodeMapPacket(p.data, &entries)) {
            for (const auto& e : entries) {
                s += " " + MacStr(e.mac) + "=node" + std::to_string(e.node_id);
            }
        } else {
            s += " (unparsable, " + std::to_string(p.data.size()) + " bytes: " + Hex(p.data.data(), p.data.size()) + ")";
        }
        Say("NodeMap", s, 6);
        break;
    }
    case WifiPacket::Type::Data: {
        EtherType et;
        if (!GetFrameEtherType(p.data, &et)) {
            Say("Data?", "unknown ethertype, " + std::to_string(p.data.size()) + " bytes");
            break;
        }
        if (et == EtherType::EAPoL) {
            u16 t = 0;
            GetEapolFrameType(p.data, &t);
            if (t == kEapolLogoffMagic) {
                EapolLogoff l;
                if (ParseEapolLogoffFrame(p.data, &l)) {
                    std::string s = "from " + MacStr(p.transmitter_address) + " assigned=" +
                                    std::to_string(l.assigned_node_id) + " nodes " +
                                    std::to_string(l.connected_nodes) + "/" + std::to_string(l.max_nodes) + ":";
                    for (const auto& n : l.nodes) {
                        if (n.network_node_id) {
                            s += " #" + std::to_string(n.network_node_id) + ":" + Name(n.username);
                        }
                    }
                    Say("EAPoL-Off", s, 6);
                }
            } else {
                Say("EAPoL?", "type 0x" + Hex(reinterpret_cast<const u8*>(&t), 0) + std::to_string(t));
            }
            break;
        }
        SecureDataHeader h;
        if (ParseSecureDataHeader(p.data, &h)) {
            char k[96];
            std::snprintf(k, sizeof(k), "SecureData node%u->%s ch%u", h.src_node_id,
                          h.dest_node_id == kBroadcastNetworkNodeId ? "ALL" : std::to_string(h.dest_node_id).c_str(),
                          h.data_channel);
            g_stats[k].Add(h.ActualDataSize());
            char line[256];
            std::snprintf(line, sizeof(line),
                          "node%u -> node%s ch%u payload %u bytes seq %u mgmt %u  [%s]", h.src_node_id,
                          h.dest_node_id == kBroadcastNetworkNodeId ? "ALL" : std::to_string(h.dest_node_id).c_str(),
                          h.data_channel, h.ActualDataSize(), h.sequence_number, h.is_management,
                          Hex(p.data.data() + kLlcHeaderSize + kSecureDataHeaderSize, h.ActualDataSize(), 12).c_str());
            Say("Data", line, 12);
        }
        break;
    }
    default:
        Say(TypeName(p.type), "from " + MacStr(p.transmitter_address) + " " + std::to_string(p.data.size()) + " bytes", 6);
        break;
    }
}

void PrintMembers(const char* title, const room::RoomInfo& info) {
    std::printf("%s: \"%s\", %zu/%u players, preferred \"%s\"\n", title, info.name.c_str(),
                info.members.size(), info.member_slots, info.preferred_game.c_str());
    for (const auto& m : info.members) {
        std::printf("   - %-14s %s  game=\"%s\" titleid=%016llX\n", m.nickname.c_str(),
                    MacStr(m.mac).c_str(), m.game_name.c_str(), static_cast<unsigned long long>(m.game_id));
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <host> [port] [seconds] [nickname]\n", argv[0]);
        return 2;
    }
    RoomClient::Config config;
    config.host = argv[1];
    config.port = argc > 2 ? static_cast<u16>(std::atoi(argv[2])) : room::kDefaultPort;
    const int seconds = argc > 3 ? std::atoi(argv[3]) : 30;
    config.nickname = argc > 4 ? argv[4] : "AzlObserver";
    config.console_id_hash = "azl-observer-" + config.nickname;
    config.game_name = "AzaharLink observer";

    RoomClient client;
    client.SetWifiCallback(OnPacket);
    g_start_ms = platform::NowMs();
    client.Start(config);
    for (int i = 0; i < 1000 && !client.joined(); ++i) {
        if (client.state() == RoomClient::State::Idle && client.error() != RoomClient::Error::None) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!client.joined()) {
        std::printf("NOT JOINED: %s\n", RoomClient::ErrorName(client.error()));
        return 1;
    }
    PrintMembers("Joined", client.room_info());
    std::printf("Observing for %d s (sending nothing)...\n\n", seconds);
    g_start_ms = platform::NowMs();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::printf("\n=== summary over %d s ===\n", seconds);
        for (const auto& kv : g_stats) {
            std::printf("  %-34s %6llu pkts  %8llu bytes  size %zu..%zu  (%.1f pkt/s)\n", kv.first.c_str(),
                        static_cast<unsigned long long>(kv.second.packets),
                        static_cast<unsigned long long>(kv.second.bytes), kv.second.min_size,
                        kv.second.max_size, kv.second.packets / static_cast<double>(seconds));
        }
        if (g_stats.empty()) {
            std::printf("  (no frames were relayed to me: nobody is hosting/using local wireless right now)\n");
        }
    }
    PrintMembers("\nRoom at the end", client.room_info());
    client.Stop();
    return 0;
}
