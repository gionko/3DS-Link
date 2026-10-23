// Differential test: AzaharLink's frame code vs. the ORIGINAL Azahar sources (uds_beacon.cpp,
// uds_data.cpp, uds_connection.cpp), compiled unmodified with shim headers (see run.sh).
// Every frame must match byte for byte, and each side must parse the other's output.
#include <array>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "core/hle/service/nwm/nwm_uds.h"
#include "core/hle/service/nwm/uds_beacon.h"
#include "core/hle/service/nwm/uds_connection.h"
#include "core/hle/service/nwm/uds_data.h"

#include "azl/uds_frames.hpp"

namespace A = Service::NWM;

static int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond);                           \
        }                                                                                          \
    } while (0)

static std::mt19937 rng(12345);
static unsigned R(unsigned n) {
    return rng() % n;
}

static azl::NodeInfo RandomNode(azl::u16 id) {
    azl::NodeInfo n;
    n.friend_code_seed = (static_cast<azl::u64>(rng()) << 32) | rng();
    for (auto& c : n.username) {
        c = static_cast<azl::u16>(rng());
    }
    n.network_node_id = id;
    return n;
}

static A::NodeInfo ToAzahar(const azl::NodeInfo& n) {
    A::NodeInfo a{};
    a.friend_code_seed = n.friend_code_seed;
    for (int i = 0; i < 10; ++i) {
        a.username[i] = n.username[i];
    }
    a.network_node_id = n.network_node_id;
    return a;
}

static azl::NetworkInfo RandomNetworkInfo(unsigned max_nodes, unsigned app_size) {
    azl::NetworkInfo ni;
    for (auto& b : reinterpret_cast<unsigned char(&)[sizeof(ni)]>(ni)) {
        b = static_cast<unsigned char>(rng()); // random padding too: both sides must copy it alike
    }
    std::memcpy(ni.oui_value, azl::kNintendoOui, 3);
    ni.oui_type = 21;
    ni.max_nodes = static_cast<azl::u8>(max_nodes);
    ni.total_nodes = static_cast<azl::u8>(1 + R(max_nodes));
    ni.application_data_size = static_cast<azl::u8>(app_size);
    return ni;
}

static bool SameBytes(const std::vector<azl::u8>& a, const std::vector<azl::u8>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

int main() {
    // ---- Beacons -------------------------------------------------------------------------
    for (unsigned max_nodes : {2u, 3u, 4u, 8u, 15u, 16u}) {
        for (unsigned app_size : {0u, 1u, 77u, 200u}) {
            for (int rep = 0; rep < 5; ++rep) {
                azl::NetworkInfo ni = RandomNetworkInfo(max_nodes, app_size);
                std::vector<azl::NodeInfo> nodes;
                A::NodeList anodes;
                for (unsigned i = 0; i < max_nodes; ++i) {
                    azl::NodeInfo n = (R(3) == 0) ? azl::NodeInfo{} : RandomNode(static_cast<azl::u16>(i + 1));
                    nodes.push_back(n);
                    anodes.push_back(ToAzahar(n));
                }
                A::NetworkInfo ani;
                static_assert(sizeof(ani) == sizeof(ni));
                std::memcpy(&ani, &ni, sizeof(ni));

                const auto mine = azl::GenerateBeaconFrame(ni, nodes);
                const auto ref = A::GenerateBeaconFrame(ani, anodes);
                CHECK(SameBytes(mine, ref));
                if (!SameBytes(mine, ref)) {
                    std::fprintf(stderr, "  beacon mismatch: max_nodes=%u app=%u (%zu vs %zu bytes)\n",
                                 max_nodes, app_size, mine.size(), ref.size());
                }

                // Decrypt cross-check
                std::vector<azl::u8> blob(0x12 + 0x1E * max_nodes);
                for (auto& b : blob) {
                    b = static_cast<azl::u8>(rng());
                }
                std::vector<azl::u8> blob2 = blob;
                azl::DecryptBeacon(ni, blob.data(), blob.size());
                A::DecryptBeacon(ani, blob2);
                CHECK(SameBytes(blob, blob2));
            }
        }
    }

    // ---- Authentication / association ------------------------------------------------------
    for (auto seq : {azl::AuthSeq::Seq1, azl::AuthSeq::Seq2}) {
        const auto mine = azl::GenerateAuthenticationFrame(seq);
        const auto ref = A::GenerateAuthenticationFrame(static_cast<A::AuthenticationSeq>(seq));
        CHECK(SameBytes(mine, ref));
        azl::AuthSeq parsed;
        CHECK(azl::GetAuthenticationSeqNumber(ref, &parsed) && parsed == seq);
        CHECK(A::GetAuthenticationSeqNumber(mine) == static_cast<A::AuthenticationSeq>(seq));
    }
    for (int i = 0; i < 50; ++i) {
        const azl::u16 aid = static_cast<azl::u16>(R(0x3FFF));
        const azl::u32 nid = rng();
        const auto mine = azl::GenerateAssocResponseFrame(0, aid, nid);
        const auto ref = A::GenerateAssocResponseFrame(A::AssocStatus::Successful, aid, nid);
        CHECK(SameBytes(mine, ref));
        azl::u16 st, id;
        CHECK(azl::GetAssociationResult(ref, &st, &id) && st == 0 && id == aid);
        auto [rst, rid] = A::GetAssociationResult(mine);
        CHECK(rst == A::AssocStatus::Successful && rid == aid);
    }

    // ---- Data frames ----------------------------------------------------------------------
    for (int i = 0; i < 100; ++i) {
        std::vector<azl::u8> payload(R(0x5C7));
        for (auto& b : payload) {
            b = static_cast<azl::u8>(rng());
        }
        const azl::u8 ch = static_cast<azl::u8>(R(256));
        const azl::u16 dest = static_cast<azl::u16>(rng()), src = static_cast<azl::u16>(rng()),
                       seq = static_cast<azl::u16>(rng());
        const auto mine = azl::GenerateDataPayload(payload.data(), payload.size(), ch, dest, src, seq);
        const auto ref = A::GenerateDataPayload(payload, ch, dest, src, seq);
        CHECK(SameBytes(mine, ref));

        azl::SecureDataHeader h;
        CHECK(azl::ParseSecureDataHeader(ref, &h));
        const auto rh = A::ParseSecureDataHeader(mine);
        CHECK(h.dest_node_id == rh.dest_node_id && h.src_node_id == rh.src_node_id);
        CHECK(h.data_channel == rh.data_channel && h.sequence_number == rh.sequence_number);
        CHECK(h.protocol_size == rh.protocol_size && h.securedata_size == rh.securedata_size);
        CHECK(h.ActualDataSize() == rh.GetActualDataSize() && h.ActualDataSize() == payload.size());
        azl::EtherType et;
        CHECK(azl::GetFrameEtherType(ref, &et) && et == azl::EtherType::SecureData);
        CHECK(A::GetFrameEtherType(mine) == A::EtherType::SecureData);
    }

    // ---- EAPoL ----------------------------------------------------------------------------
    for (int i = 0; i < 50; ++i) {
        const azl::NodeInfo node = RandomNode(static_cast<azl::u16>(R(17)));
        const azl::u16 aid = static_cast<azl::u16>(rng());
        for (auto type : {azl::ConnectionType::Client, azl::ConnectionType::Spectator}) {
            const auto mine = azl::GenerateEapolStartFrame(aid, type, node);
            const auto ref = A::GenerateEAPoLStartFrame(aid, static_cast<A::ConnectionType>(type),
                                                        ToAzahar(node));
            CHECK(SameBytes(mine, ref));
            azl::EapolStart es;
            CHECK(azl::ParseEapolStartFrame(ref, &es));
            const auto rs = A::DeserializeEAPolStartPacket(mine);
            CHECK(es.association_id == rs.association_id);
            CHECK(es.connection_type == static_cast<azl::u8>(rs.connection_type));
            const A::NodeInfo rnode = A::DeserializeNodeInfo(rs.node);
            CHECK(es.node.friend_code_seed == rnode.friend_code_seed);
            CHECK(es.node.username[3] == rnode.username[3]);
            azl::u16 t;
            CHECK(azl::GetEapolFrameType(ref, &t) && t == azl::kEapolStartMagic);
            CHECK(A::GetEAPoLFrameType(mine) == A::EAPoLStartMagic);
        }

        const unsigned max_nodes = 2 + R(15);
        std::vector<azl::NodeInfo> nodes;
        A::NodeList anodes;
        for (unsigned k = 0; k < max_nodes; ++k) {
            azl::NodeInfo n = (R(3) == 0) ? azl::NodeInfo{} : RandomNode(static_cast<azl::u16>(k + 1));
            nodes.push_back(n);
            anodes.push_back(ToAzahar(n));
        }
        const azl::u16 assigned = static_cast<azl::u16>(1 + R(16));
        const azl::u8 total = static_cast<azl::u8>(1 + R(max_nodes));
        const auto mine = azl::GenerateEapolLogoffFrame(assigned, nodes, static_cast<azl::u8>(max_nodes), total);
        const auto ref = A::GenerateEAPoLLogoffFrame(A::MacAddress{1, 2, 3, 4, 5, 6}, assigned, anodes,
                                                     static_cast<azl::u8>(max_nodes), total);
        CHECK(SameBytes(mine, ref));
        azl::EapolLogoff el;
        CHECK(azl::ParseEapolLogoffFrame(ref, &el));
        const auto rl = A::ParseEAPoLLogoffFrame(mine);
        CHECK(el.assigned_node_id == rl.assigned_node_id);
        CHECK(el.connected_nodes == rl.connected_nodes && el.max_nodes == rl.max_nodes);
        for (unsigned k = 0; k < 16; ++k) {
            CHECK(el.nodes[k].network_node_id == rl.nodes[k].network_node_id);
            CHECK(el.nodes[k].friend_code_seed == rl.nodes[k].friend_code_seed);
        }
    }

    // ---- NodeMap ---------------------------------------------------------------------------
    // The packet by which a client learns the MACs of the other nodes - and, since 0.1.16, what the foreign-lobby filter of a
    // client takes as "one of the nodes of this network". NWM_UDS::BroadcastNodeMap() and ::HandleNodeMapPacket() are members
    // of a class that is not built here, so their serialisation is copied from nwm_uds.cpp of the same release (count as a
    // host std::size_t, then {MAC, node id} per entry, in the order of the map) and held against ours both ways.
    for (int i = 0; i < 60; ++i) {
        std::vector<azl::NodeMapEntry> entries;
        const unsigned n = R(17);
        for (unsigned k = 0; k < n; ++k) {
            azl::NodeMapEntry e;
            for (auto& b : e.mac) {
                b = static_cast<azl::u8>(rng());
            }
            e.node_id = static_cast<azl::u16>(1 + R(16));
            entries.push_back(e);
        }
        // BroadcastNodeMap: resize, memcpy the count, then each MAC and node id
        std::size_t num_entries = entries.size();
        std::vector<azl::u8> ref(sizeof(num_entries) + (6 + sizeof(azl::u16)) * num_entries);
        std::memcpy(ref.data(), &num_entries, sizeof(num_entries));
        std::size_t offset = sizeof(num_entries);
        for (const auto& e : entries) {
            std::memcpy(ref.data() + offset, e.mac.data(), 6);
            std::memcpy(ref.data() + offset + 6, &e.node_id, sizeof(e.node_id));
            offset += 6 + sizeof(e.node_id);
        }
        CHECK(SameBytes(azl::GenerateNodeMapPacket(entries), ref));
        // HandleNodeMapPacket: the count, then the pairs
        std::vector<azl::NodeMapEntry> parsed;
        CHECK(azl::ParseNodeMapPacket(ref, &parsed));
        std::size_t count = 0;
        std::memcpy(&count, ref.data(), sizeof(count));
        CHECK(count == entries.size() && parsed.size() == entries.size());
        offset = sizeof(count);
        for (std::size_t k = 0; k < count && k < parsed.size(); ++k) {
            std::array<azl::u8, 6> mac;
            azl::u16 id;
            std::memcpy(&mac, ref.data() + offset, sizeof(mac));
            std::memcpy(&id, ref.data() + offset + sizeof(mac), sizeof(id));
            CHECK(parsed[k].mac == mac && parsed[k].node_id == id);
            offset += sizeof(mac) + sizeof(id);
        }
    }

    std::printf("differential: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
