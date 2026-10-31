// Host-side unit tests for the portable core (crypto, room protocol, frames, UDS engine).
// Build & run: see Tests/run.sh
#include <algorithm>
#include <atomic>
#include <chrono>
#include <enet/enet.h>

#include "azl/room_client.hpp"
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
// libstdc++'s <regex> trips -Wmaybe-uninitialized inside its own headers at -O1.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#include <regex>
#pragma GCC diagnostic pop
#include <string>
#include <thread>

#include "azl/crypto.hpp"
#include "azl/log.hpp"
#include "azl/ndm_hle.hpp"
#include "azl/room_members.hpp"
#include "azl/room_proto.hpp"
#include "azl/ticks.h"
#include "azl/tx_queue.hpp"
#include "azl/stats.hpp"
#include "azl/uds_engine.hpp"
#include "azl/uds_frames.hpp"

using namespace azl;

#include "support.hpp"
#include "mock_room.hpp"
#include "azahar_peer.hpp"

// ---------------------------------------------------------------------------------------------
// Crypto
// ---------------------------------------------------------------------------------------------
static void TestCrypto() {
    std::printf("crypto\n");
    u8 md[16];
    Md5(reinterpret_cast<const u8*>("abc"), 3, md);
    CHECK_EQ(Hex(md, 16), std::string("900150983cd24fb0d6963f7d28e17f72"));
    Md5(reinterpret_cast<const u8*>(""), 0, md);
    CHECK_EQ(Hex(md, 16), std::string("d41d8cd98f00b204e9800998ecf8427e"));
    // 56..64 byte inputs exercise the two-block padding path
    const char* long_msg = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
    Md5(reinterpret_cast<const u8*>(long_msg), 80, md);
    CHECK_EQ(Hex(md, 16), std::string("57edf4a22be3c955ac49da2e2107b67a"));

    u8 sha[20];
    Sha1(reinterpret_cast<const u8*>("abc"), 3, sha);
    CHECK_EQ(Hex(sha, 20), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    Sha1(reinterpret_cast<const u8*>(""), 0, sha);
    CHECK_EQ(Hex(sha, 20), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    const char* two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    Sha1(reinterpret_cast<const u8*>(two_block), 56, sha);
    CHECK_EQ(Hex(sha, 20), std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));

    // FIPS-197 appendix C.1: AES-128 single block (via CTR over a zero plaintext)
    {
        const auto key = FromHex("000102030405060708090a0b0c0d0e0f");
        const auto pt = FromHex("00112233445566778899aabbccddeeff");
        u8 zeros[16] = {};
        Aes128CtrXor(key.data(), pt.data(), zeros, 16);
        CHECK_EQ(Hex(zeros, 16), std::string("69c4e0d86a7b0430d8cdb78070b4c55a"));
    }
    // NIST SP 800-38A F.5.1 CTR-AES128 (two blocks, counter increment)
    {
        const auto key = FromHex("2b7e151628aed2a6abf7158809cf4f3c");
        const auto ctr = FromHex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
        auto data = FromHex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
        Aes128CtrXor(key.data(), ctr.data(), data.data(), data.size());
        CHECK_EQ(Hex(data.data(), data.size()),
                 std::string("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"));
        // and it is its own inverse
        Aes128CtrXor(key.data(), ctr.data(), data.data(), data.size());
        CHECK_EQ(Hex(data.data(), data.size()),
                 std::string("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"));
    }
}

// ---------------------------------------------------------------------------------------------
// Room protocol
// ---------------------------------------------------------------------------------------------
static void TestProto() {
    std::printf("room protocol\n");
    // Join request: id, nick, console id, mac, version(be32), password, token
    const Mac pref = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    auto join = room::EncodeJoinRequest("Gionko", "3ds-aabb", pref, "pw", "");
    CHECK_EQ(join[0], room::IdJoinRequest);
    CHECK_EQ(Hex(join.data() + 1, 4), std::string("00000006"));         // len("Gionko")
    CHECK_EQ(std::string(join.begin() + 5, join.begin() + 11), std::string("Gionko"));
    // ... 4+6 (nick) + 4+8 (console id) = offset 23 for the mac
    CHECK_EQ(Hex(join.data() + 1 + 10 + 12, 6), std::string("ffffffffffff"));
    CHECK_EQ(Hex(join.data() + 1 + 10 + 12 + 6, 4), std::string("00000004")); // network version 4
    CHECK_EQ(join.size(), std::size_t(1 + 10 + 12 + 6 + 4 + 6 + 4));

    auto gi = room::EncodeSetGameInfo("Zelda", 0x0004000000164800ULL);
    CHECK_EQ(gi[0], room::IdSetGameInfo);
    CHECK_EQ(Hex(gi.data() + 1 + 4 + 5, 8), std::string("0004000000164800"));

    // Wifi packet round trip
    WifiPacket p;
    p.type = WifiPacket::Type::Data;
    p.channel = 11;
    p.transmitter_address = {1, 2, 3, 4, 5, 6};
    p.destination_address = kBroadcastMac;
    p.data = {9, 8, 7, 6, 5};
    auto enc = room::EncodeWifiPacket(p);
    CHECK_EQ(enc[0], room::IdWifiPacket);
    CHECK_EQ(enc[1], 1);  // type Data
    CHECK_EQ(enc[2], 11); // channel
    CHECK_EQ(Hex(enc.data() + 3, 12), std::string("010203040506ffffffffffff"));
    CHECK_EQ(Hex(enc.data() + 15, 4), std::string("00000005"));
    WifiPacket q;
    CHECK(room::DecodeWifiPacket(enc.data(), enc.size(), &q));
    CHECK(q.type == p.type && q.channel == 11 && q.data == p.data);
    CHECK(q.transmitter_address == p.transmitter_address);
    // truncated packets must be rejected, not crash
    for (std::size_t n = 0; n < enc.size(); ++n) {
        WifiPacket t;
        CHECK(!room::DecodeWifiPacket(enc.data(), n, &t));
    }

    // Room information, hand-built exactly like Room::BroadcastRoomInformation
    room::PacketWriter w;
    w.U8(room::IdRoomInformation);
    w.String("My Room");
    w.String("desc");
    w.U32(16);
    w.U16(24872);
    w.String("Zelda");
    w.String("host");
    w.U32(2);
    for (int i = 0; i < 2; ++i) {
        w.String(i ? "bob" : "alice");
        w.MacAddress({0, 0x1F, 0x32, 0, 0, static_cast<u8>(i + 1)});
        w.String("game");
        w.U64(0x1234 + i);
        w.String("u");
        w.String("d");
        w.String("a");
    }
    room::RoomInfo info;
    CHECK(room::DecodeRoomInformation(w.data().data(), w.data().size(), &info));
    CHECK_EQ(info.members.size(), std::size_t(2));
    CHECK_EQ(info.member_slots, 16u);
    CHECK(info.members[1].nickname == "bob");
    CHECK_EQ(info.members[1].game_id, u64(0x1235));
    // garbage member counts must not allocate / loop forever
    room::PacketWriter bad;
    bad.U8(room::IdRoomInformation);
    bad.String("a");
    bad.String("b");
    bad.U32(1);
    bad.U16(1);
    bad.String("c");
    bad.String("d");
    bad.U32(0x7FFFFFFF);
    CHECK(!room::DecodeRoomInformation(bad.data().data(), bad.data().size(), &info));

    Mac m;
    const u8 js[] = {room::IdJoinSuccess, 0, 0x1F, 0x32, 1, 2, 3};
    CHECK(room::DecodeJoinSuccess(js, sizeof(js), &m));
    CHECK_EQ(Hex(m.data(), 6), std::string("001f32010203"));
}

// ---------------------------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------------------------
static void TestFrames() {
    std::printf("frames\n");
    // Auth / assoc
    auto auth = GenerateAuthenticationFrame(AuthSeq::Seq1);
    CHECK_EQ(Hex(auth.data(), auth.size()), std::string("000001000000"));
    AuthSeq seq;
    CHECK(GetAuthenticationSeqNumber(auth, &seq) && seq == AuthSeq::Seq1);
    auto assoc = GenerateAssocResponseFrame(0, 1, 0xDEADBEEF);
    // capabilities 0x0431 LE, status 0, association id 1 | 0xC000 (LE)
    CHECK_EQ(Hex(assoc.data(), 6), std::string("3104000001c0"));
    CHECK_EQ(std::string(assoc.begin() + 8, assoc.end()), std::string("DEADBEEF"));
    u16 st, aid;
    CHECK(GetAssociationResult(assoc, &st, &aid) && st == 0 && aid == 1);

    // Data payload layout
    const u8 payload[] = {1, 2, 3, 4, 5};
    auto data = GenerateDataPayload(payload, 5, 7, 2, 1, 0);
    CHECK_EQ(Hex(data.data(), 8), std::string("aaaa03000000876d"));
    // protocol_size 19, pad, securedata_size 15, mgmt 0, channel 7, seq 0, dest 2, src 1
    CHECK_EQ(Hex(data.data() + 8, 14), std::string("00130000000f0007000000020001"));
    SecureDataHeader sh;
    CHECK(ParseSecureDataHeader(data, &sh));
    CHECK_EQ(sh.ActualDataSize(), 5u);
    CHECK_EQ(sh.dest_node_id, 2);
    CHECK_EQ(sh.src_node_id, 1);
    CHECK_EQ(sh.data_channel, 7);
    EtherType et;
    CHECK(GetFrameEtherType(data, &et) && et == EtherType::SecureData);

    // EAPoL start/logoff sizes and field offsets
    auto start = GenerateEapolStartFrame(1, ConnectionType::Client, MakeNode(0x1122334455667788ULL, "Bob", 5));
    CHECK_EQ(start.size(), kLlcHeaderSize + kEapolStartSize);
    u16 t;
    CHECK(GetEapolFrameType(start, &t) && t == kEapolStartMagic);
    EapolStart es;
    CHECK(ParseEapolStartFrame(start, &es));
    CHECK_EQ(es.node.friend_code_seed, u64(0x1122334455667788ULL));
    CHECK_EQ(es.node.username[0], u16('B'));
    CHECK_EQ(es.node.network_node_id, 0); // left uninitialised like Azahar
    CHECK_EQ(es.connection_type, 1);

    std::vector<NodeInfo> nodes(4);
    nodes[0] = MakeNode(1, "Host", 1);
    nodes[2] = MakeNode(3, "Third", 3);
    auto logoff = GenerateEapolLogoffFrame(3, nodes, 4, 2);
    CHECK_EQ(logoff.size(), kLlcHeaderSize + kEapolLogoffSize);
    CHECK(GetEapolFrameType(logoff, &t) && t == kEapolLogoffMagic);
    EapolLogoff el;
    CHECK(ParseEapolLogoffFrame(logoff, &el));
    CHECK_EQ(el.assigned_node_id, 3);
    CHECK_EQ(el.connected_nodes, 2);
    CHECK_EQ(el.max_nodes, 4);
    CHECK_EQ(el.nodes[0].network_node_id, 1);
    CHECK_EQ(el.nodes[1].network_node_id, 0);
    CHECK_EQ(el.nodes[2].friend_code_seed, u64(3));

    // Truncated frames are rejected
    for (std::size_t n = 0; n < logoff.size(); n += 7) {
        std::vector<u8> cut(logoff.begin(), logoff.begin() + n);
        EapolLogoff x;
        CHECK(!ParseEapolLogoffFrame(cut, &x));
    }

    // NodeMap: count is 8 bytes LE (host size_t in Azahar)
    auto nm = GenerateNodeMapPacket({{{1, 2, 3, 4, 5, 6}, 2}, {{6, 5, 4, 3, 2, 1}, 3}});
    CHECK_EQ(nm.size(), std::size_t(8 + 16));
    CHECK_EQ(Hex(nm.data(), 8), std::string("0200000000000000"));
    CHECK_EQ(Hex(nm.data() + 8, 8), std::string("0102030405060200"));
    std::vector<NodeMapEntry> ent;
    CHECK(ParseNodeMapPacket(nm, &ent) && ent.size() == 2 && ent[1].node_id == 3);
    nm[0] = 0xFF; // absurd count
    CHECK(!ParseNodeMapPacket(nm, &ent));

    // Beacon: structure matches what libctru's usd_parsebeacon expects
    NetworkInfo ni = MakeNetworkInfo(8, 10);
    std::memcpy(ni.oui_value, kNintendoOui, 3);
    ni.oui_type = 21;
    ni.host_mac_address = {0x00, 0x1F, 0x32, 0xAA, 0xBB, 0xCC};
    std::vector<NodeInfo> bn(8);
    bn[0] = MakeNode(0xABCDEF0102030405ULL, "HostName", 1);
    auto beacon = GenerateBeaconFrame(ni, bn);
    CHECK_EQ(beacon[10], 0x31); // capabilities 0x0431 LE
    CHECK_EQ(beacon[11], 0x04);
    // walk the tags like libctru does
    std::size_t pos = 12;
    const u8* tag_ptr[3] = {};
    std::size_t tag_len[3] = {};
    while (pos + 2 <= beacon.size()) {
        const u8 id = beacon[pos], len = beacon[pos + 1];
        pos += 2;
        CHECK(pos + len <= beacon.size());
        if (id == 0xdd && std::memcmp(&beacon[pos], kNintendoOui, 3) == 0) {
            const u8 type = beacon[pos + 3];
            const int idx = type == 21 ? 0 : type == 24 ? 1 : type == 25 ? 2 : -1;
            if (idx >= 0) {
                tag_ptr[idx] = &beacon[pos];
                tag_len[idx] = len;
            }
        }
        pos += len;
    }
    CHECK_EQ(pos, beacon.size());
    CHECK(tag_ptr[0] && tag_ptr[1] && tag_ptr[2]); // 8 nodes -> 0x12+0xF0=258 > 0xFA: two tags
    CHECK_EQ(tag_len[0], std::size_t(0x34 + 10));
    CHECK_EQ(tag_ptr[0][0x33], 10);                  // appdata size
    CHECK_EQ(tag_len[1], std::size_t(4 + 0xFA));
    CHECK_EQ(tag_len[2], std::size_t(4 + 258 - 0xFA));
    // SHA1 stored in the network info tag covers the tag minus its header
    {
        std::vector<u8> tag(beacon.begin() + (tag_ptr[0] - beacon.data()) - 2,
                            beacon.begin() + (tag_ptr[0] - beacon.data()) + tag_len[0]);
        std::memset(tag.data() + 0x21, 0, 20);
        u8 h[20];
        Sha1(tag.data() + 2, tag.size() - 2, h);
        CHECK_EQ(Hex(h, 20), Hex(tag_ptr[0] + 0x1F, 20) /* sha at data offset 0x1F */);
    }
    // Decrypting the node list recovers the host node and the MD5
    {
        std::vector<u8> blob(tag_ptr[1] + 4, tag_ptr[1] + tag_len[1]);
        blob.insert(blob.end(), tag_ptr[2] + 4, tag_ptr[2] + tag_len[2]);
        CHECK_EQ(blob.size(), std::size_t(258));
        DecryptBeacon(ni, blob.data(), blob.size());
        u8 md[16];
        Md5(blob.data() + 16, blob.size() - 16, md);
        CHECK_EQ(Hex(md, 16), Hex(blob.data(), 16));
        CHECK_EQ(LoadBE64(blob.data() + 0x12), u64(0xABCDEF0102030405ULL));
        CHECK_EQ(LoadBE16(blob.data() + 0x12 + 8), u16('H'));
        CHECK_EQ(LoadBE16(blob.data() + 0x12 + 0x1C), u16(1));
    }
    // The tags the engine reads from a beacon, and the node set it announces (the node ids: Azahar
    // leaves the bitmask of the list empty)
    {
        const BeaconTags tags = FindBeaconTags(beacon);
        CHECK(tags.network && tags.nodes[0] && tags.nodes[1]);
        CHECK_EQ(tags.network_size, 0x34u + 10);
        CHECK_EQ(LoadBE32(tags.network + 4), ni.wlan_comm_id()); // the vendor prefix a scan filters on
        CHECK_EQ(tags.network[8], ni.id);
        u16 mask = 0;
        CHECK(GetBeaconNodeMask(tags, ni.host_mac_address, &mask));
        CHECK_EQ(mask, 0x0001); // the host
        bn[2] = MakeNode(3, "Third", 3);
        bn[5] = MakeNode(6, "Sixth", 6);
        const std::vector<u8> bigger = GenerateBeaconFrame(ni, bn);
        CHECK(GetBeaconNodeMask(FindBeaconTags(bigger), ni.host_mac_address, &mask));
        CHECK_EQ(mask, 0x0025);
        // the key depends on the host's MAC: the wrong one decrypts to garbage, whose MD5 does not match
        CHECK(!GetBeaconNodeMask(FindBeaconTags(bigger), Mac{1, 2, 3, 4, 5, 6}, &mask));
        // a beacon cut inside a tag, or without a node list, is not read
        const std::vector<u8> cut(beacon.begin(), beacon.begin() + 40);
        CHECK(FindBeaconTags(cut).network == nullptr);
        CHECK(!GetBeaconNodeMask(FindBeaconTags(cut), ni.host_mac_address, &mask));
        const std::vector<u8> no_list(beacon.begin(), beacon.begin() + (tags.nodes[0] - beacon.data()) - 2);
        CHECK(FindBeaconTags(no_list).network != nullptr && FindBeaconTags(no_list).nodes[0] == nullptr);
        CHECK(!GetBeaconNodeMask(FindBeaconTags(no_list), ni.host_mac_address, &mask));
    }
}

// ---------------------------------------------------------------------------------------------
// Engine: two/three engines connected through a mock room that routes like Azahar's room server
// (MockRoom / Console live in mock_room.hpp; every frame they send is checked by the Azahar-peer
// rule checker in azahar_rules.hpp)
// ---------------------------------------------------------------------------------------------

static void TestEngine() {
    std::printf("uds engine (host + 2 clients through mock room)\n");
    g_rules.Reset();
    MockRoom room;
    Console host(room, 1), alice(room, 2), bob(room, 3);

    u8 raw[kNodeInfoSize];
    u32 ev;
    MakeNode(0x1111, "Host", 0).Store(raw);
    NodeInfo n;
    n.Load(raw);
    CHECK_EQ(host.engine->Initialize(n, kTestSharedMemory, kTestVersion, &ev), 0u);
    MakeNode(0x2222, "Alice", 0).Store(raw);
    n.Load(raw);
    CHECK_EQ(alice.engine->Initialize(n, kTestSharedMemory, kTestVersion, &ev), 0u);
    MakeNode(0x3333, "Bob", 0).Store(raw);
    n.Load(raw);
    CHECK_EQ(bob.engine->Initialize(n, kTestSharedMemory, kTestVersion, &ev), 0u);

    // Host creates a network
    NetworkInfo ni = MakeNetworkInfo(4, 6);
    CHECK_EQ(host.engine->BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni)), 0u);
    CHECK(host.engine->status() == NetworkStatus::ConnectedAsHost);
    ConnectionStatus cs = host.engine->GetConnectionStatus();
    CHECK_EQ(cs.total_nodes, 1);
    CHECK_EQ(cs.max_nodes, 4);
    CHECK_EQ(cs.network_node_id, 1);
    CHECK_EQ(cs.node_bitmask, 1);
    u8 chan = 0;
    host.engine->GetChannel(&chan);
    CHECK_EQ(chan, 11);

    // Host bind on channel 1 (any sender)
    u32 host_bind_ev = 0;
    CHECK_EQ(host.engine->Bind(2, 0x5F4, 1, kBroadcastNetworkNodeId, &host_bind_ev), 0u);
    u32 bad_ev;
    CHECK_EQ(host.engine->Bind(0, 0x5F4, 1, 0xFFFF, &bad_ev), res::Make(res::NotAuthorized, res::WrongArgument, res::Usage));
    CHECK_EQ(host.engine->Bind(4, 0x10, 2, 0xFFFF, &bad_ev), 0xC86113F3u); // ring too small: out of resource

    // Beacons reach the scanning clients
    host.engine->Tick(platform::NowMs() + 100000);
    room.WaitIdle();
    std::vector<u8> scan(0x4000);
    CHECK_EQ(alice.engine->RecvBeaconBroadcastData(scan.data(), scan.size(), kBroadcastMac), 0u);
    CHECK_EQ(LoadLE32(scan.data() + 0), scan.size());
    CHECK_EQ(LoadLE32(scan.data() + 8), 1u); // one network
    const u8* entry = scan.data() + kBeaconDataReplyHeaderSize;
    const u32 entry_size = LoadLE32(entry);
    CHECK_EQ(entry[5], 11);
    CHECK_EQ(Hex(entry + 8, 6), Hex(host.mac.data(), 6));
    CHECK_EQ(LoadLE32(entry + 0x18), u32(kBeaconEntryHeaderSize));
    CHECK_EQ(LoadLE32(scan.data() + 4), kBeaconDataReplyHeaderSize + entry_size);

    // Re-assemble the scanned network info and decrypt the host's node like a game would
    std::vector<u8> beacon(entry + kBeaconEntryHeaderSize, entry + entry_size);
    NetworkInfo scanned;
    std::memset(&scanned, 0, sizeof(scanned));
    const u8* tags[3] = {};
    std::size_t tag_len[3] = {};
    for (std::size_t pos = 12; pos + 2 <= beacon.size();) {
        const u8 id = beacon[pos], len = beacon[pos + 1];
        pos += 2;
        if (id == 0xdd && std::memcmp(&beacon[pos], kNintendoOui, 3) == 0) {
            const u8 type = beacon[pos + 3];
            const int idx = type == 21 ? 0 : type == 24 ? 1 : type == 25 ? 2 : -1;
            if (idx >= 0) {
                tags[idx] = &beacon[pos];
                tag_len[idx] = len;
            }
        }
        pos += len;
    }
    CHECK(tags[0] && tags[1]);
    std::memcpy(scanned.oui_value, tags[0], 0x1F);
    scanned.application_data_size = tags[0][0x33];
    std::memcpy(scanned.application_data, tags[0] + 0x34, scanned.application_data_size);
    scanned.channel = entry[5];
    std::memcpy(scanned.host_mac_address.data(), entry + 8, 6);
    CHECK_EQ(scanned.wlan_comm_id(), 0x0010CAFEu);
    CHECK_EQ(scanned.network_id(), 0xDEADBEEFu);
    CHECK_EQ(scanned.application_data_size, 6);
    CHECK_EQ(scanned.application_data[3], 0xA3);

    u8 tmp0[0xFE] = {}, tmp1[0xFE] = {};
    std::memcpy(tmp0, tags[0 + 1], tag_len[1]);
    if (tags[2]) {
        std::memcpy(tmp1, tags[2], tag_len[2]);
    }
    std::vector<u8> nodes_out;
    CHECK_EQ(alice.engine->DecryptBeaconData(scanned, tmp0, sizeof(tmp0), tmp1, sizeof(tmp1), &nodes_out), 0u);
    CHECK_EQ(nodes_out.size(), std::size_t(0x280));
    NodeInfo first;
    first.Load(nodes_out.data());
    CHECK_EQ(first.friend_code_seed, u64(0x1111));
    CHECK_EQ(first.username[0], u16('H'));
    CHECK_EQ(first.network_node_id, 1);

    // Alice connects (blocking call; the room thread delivers the handshake frames)
    CHECK_EQ(alice.engine->ConnectToNetwork(reinterpret_cast<u8*>(&scanned), sizeof(scanned), 1), 0u);
    CHECK(alice.engine->status() == NetworkStatus::ConnectedAsClient);
    cs = alice.engine->GetConnectionStatus();
    CHECK_EQ(cs.network_node_id, 2);
    CHECK_EQ(cs.total_nodes, 2);
    CHECK_EQ(cs.max_nodes, 4);
    CHECK_EQ(cs.node_bitmask, 3);
    NodeInfo got;
    CHECK_EQ(alice.engine->GetNodeInformation(1, &got), 0u);
    CHECK_EQ(got.friend_code_seed, u64(0x1111));
    CHECK_EQ(alice.engine->GetNodeInformation(2, &got), 0u);
    CHECK_EQ(got.friend_code_seed, u64(0x2222));
    CHECK(alice.engine->GetNodeInformation(4, &got) != 0);
    room.WaitIdle();
    cs = host.engine->GetConnectionStatus();
    CHECK_EQ(cs.total_nodes, 2);
    CHECK_EQ(cs.node_bitmask, 3);
    CHECK_EQ(cs.changed_nodes, 2); // only Alice's node (bit 1) changed since the last query

    // Bob connects too; Alice must learn about it through the broadcast Logoff frame
    CHECK_EQ(bob.engine->ConnectToNetwork(reinterpret_cast<u8*>(&scanned), sizeof(scanned), 1), 0u);
    room.WaitIdle();
    cs = bob.engine->GetConnectionStatus();
    CHECK_EQ(cs.network_node_id, 3);
    cs = alice.engine->GetConnectionStatus();
    CHECK_EQ(cs.total_nodes, 3);
    CHECK_EQ(cs.node_bitmask, 7);
    CHECK_EQ(alice.engine->GetNodeInformation(3, &got), 0u);
    CHECK_EQ(got.friend_code_seed, u64(0x3333));

    // Data: alice -> host, host -> alice, alice -> bob (direct), host broadcast
    u32 alice_ev = 0, bob_ev = 0;
    CHECK_EQ(alice.engine->Bind(2, 0x5F4, 1, kBroadcastNetworkNodeId, &alice_ev), 0u);
    CHECK_EQ(bob.engine->Bind(2, 0x5F4, 1, 1, &bob_ev), 0u); // only accepts node 1 (host)
    const u8 msg1[] = "hello host";
    CHECK_EQ(alice.engine->SendTo(1, 1, sizeof(msg1), 0, msg1, sizeof(msg1)), 0u);
    room.WaitIdle();
    u16 src = 0;
    u32 size = 0;
    auto got1 = PullAll(host, 2, &src, &size);
    CHECK_EQ(size, u32(sizeof(msg1)));
    CHECK_EQ(src, 2);
    CHECK(std::memcmp(got1.data(), msg1, sizeof(msg1)) == 0);
    // queue is now empty: size 0
    PullAll(host, 2, &src, &size);
    CHECK_EQ(size, 0u);

    const u8 msg2[] = "hi alice";
    CHECK_EQ(host.engine->SendTo(2, 1, sizeof(msg2), 0, msg2, sizeof(msg2)), 0u);
    room.WaitIdle();
    auto got2 = PullAll(alice, 2, &src, &size);
    CHECK_EQ(size, u32(sizeof(msg2)));
    CHECK_EQ(src, 1);
    CHECK(std::memcmp(got2.data(), msg2, sizeof(msg2)) == 0);

    // Broadcast flag (0x2) from the host reaches both clients; bob's bind filters by node 1
    const u8 msg3[] = "everyone";
    CHECK_EQ(host.engine->SendTo(kBroadcastNetworkNodeId, 1, sizeof(msg3), 2, msg3, sizeof(msg3)), 0u);
    room.WaitIdle();
    PullAll(alice, 2, &src, &size);
    CHECK_EQ(size, u32(sizeof(msg3)));
    PullAll(bob, 2, &src, &size);
    CHECK_EQ(size, u32(sizeof(msg3)));

    // Alice -> Bob direct (goes straight to Bob's MAC via the node map), but Bob only listens
    // to node 1, so nothing is queued for him.
    CHECK_EQ(alice.engine->SendTo(3, 1, sizeof(msg1), 0, msg1, sizeof(msg1)), 0u);
    room.WaitIdle();
    PullAll(bob, 2, &src, &size);
    CHECK_EQ(size, 0u);

    // Error paths: the firmware's three, in its order (size, state, "to myself"); a node that is not
    // there is no error (the frame is sent and nobody takes it)
    CHECK_EQ(alice.engine->SendTo(2, 1, 4, 0, msg1, 4), 0xC90113FAu); // to myself
    std::vector<u8> big(0x5C7, 0);
    CHECK_EQ(alice.engine->SendTo(1, 1, 0x5C7, 0, big.data(), big.size()), 0xE10113E9u);
    CHECK_EQ(alice.engine->SendTo(0, 1, 4, 0, msg1, 4), 0u);
    CHECK_EQ(alice.engine->SendTo(9, 1, 4, 0, msg1, 4), 0u);

    // Bob leaves; the host notices and updates its status
    CHECK_EQ(bob.engine->DisconnectNetwork(), 0u);
    CHECK(bob.engine->status() == NetworkStatus::NotConnected);
    room.WaitIdle();
    cs = host.engine->GetConnectionStatus();
    CHECK_EQ(cs.total_nodes, 2);
    CHECK_EQ(cs.node_bitmask, 3);

    // Host tears the network down; a second Destroy is "wrong state"
    CHECK_EQ(host.engine->DestroyNetwork(), 0u);
    CHECK(host.engine->status() == NetworkStatus::NotConnected);
    CHECK_EQ(host.engine->DestroyNetwork(), 0xC8A113EAu);

    CHECK_EQ(alice.engine->Shutdown(), 0u);
    CHECK(!alice.engine->initialized());
    room.WaitIdle();
    CHECK(g_rules.inspected() > 12); // the checker really saw the traffic
    CHECK_RULES();
}

static void TestConnectTimeout() {
    std::printf("uds engine (connect timeout)\n");
    // A client connecting to a host that never answers must give up with the Azahar error.
    // (The FakeKernel timeout is shortened by waiting for the real 5 s... instead we emulate with
    // a kernel whose WaitEvent returns false immediately.)
    struct ImmediateTimeout : FakeKernel {
        bool WaitEvent(u32, s64) override {
            return false;
        }
    } kernel;
    g_rules.Reset();
    UdsEngine::Transport tr;
    tr.send = [](WifiPacket& p) {
        p.transmitter_address = Mac{0, 0x1F, 0x32, 0, 0, 9};
        g_rules.Inspect(p);
    };
    tr.get_mac = [] { return Mac{0, 0x1F, 0x32, 0, 0, 9}; };
    UdsEngine engine(&kernel, tr);
    u32 ev;
    CHECK_EQ(engine.Initialize(NodeInfo{}, kTestSharedMemory, kTestVersion, &ev), 0u);
    NetworkInfo ni = MakeNetworkInfo(4, 0);
    ni.host_mac_address = {1, 2, 3, 4, 5, 6};
    CHECK_EQ(engine.ConnectToNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni), 1),
             res::Make(res::Timeout, res::Canceled, res::Status));
    CHECK(engine.status() == NetworkStatus::NotConnected);
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------
// ndm:u (F19): the faithful, session-aware emulation. Every expected value is derived from the firmware's
// ndm module 02.46.01 (addresses in ndm_hle.cpp) and from the FFE captures, none from Azahar.
// ---------------------------------------------------------------------------------------------
namespace ndmtest {

constexpr u32 H(u32 id, u32 normal, u32 translate) {
    return (id << 16) | (normal << 6) | translate;
}

// One client = one session. The request layouts are libctru's ndm.c (and the FFE captures').
struct Client {
    NdmHle& ndm;
    int s;
    u32 cmd[16];
    u32 Call(u32 hdr, u32 a = 0, u32 b = 0, u32 c = 0, u32 d = 0) {
        std::memset(cmd, 0xAA, sizeof(cmd)); // stale words, like a real command buffer
        cmd[0] = hdr;
        cmd[1] = a;
        cmd[2] = b;
        cmd[3] = c;
        cmd[4] = d;
        ndm.Dispatch(s, cmd);
        return cmd[1];
    }
    u32 Enter(u32 mode, u32 pid = 0) { return Call(H(1, 1, 2), mode, 0x20, pid); }
    u32 Leave(u32 pid = 0) { return Call(H(2, 0, 2), 0x20, pid); }
    u32 Query() {
        Call(H(3, 0, 0));
        return cmd[2];
    }
    u32 Lock(u32 pid = 0) { return Call(H(4, 0, 2), 0x20, pid); }
    u32 Unlock(u32 pid = 0) { return Call(H(5, 0, 2), 0x20, pid); }
    u32 Suspend(u32 m) { return Call(H(6, 1, 0), m); }
    u32 Resume(u32 m) { return Call(H(7, 1, 0), m); }
    u32 SuspendSched(u32 f = 0) { return Call(H(8, 1, 0), f); }
    u32 ResumeSched() { return Call(H(9, 0, 0)); }
    u32 Status(u32 d) {
        Call(H(0xD, 1, 0), d);
        return cmd[2];
    }
    u32 DaemonCount(u32 d, u32* total) {
        Call(H(0xE, 1, 0), d);
        *total = cmd[3];
        return cmd[2];
    }
    u32 SchedCount(u32* total) {
        Call(H(0xF, 0, 0));
        *total = cmd[3];
        return cmd[2];
    }
    u32 Override(u32 m) { return Call(H(0x14, 1, 0), m); }
    u32 Reset() { return Call(H(0x15, 0, 0)); }
    u32 Defaults() {
        Call(H(0x16, 0, 0));
        return cmd[2];
    }
};

} // namespace ndmtest

// FFE's sequence from the hardware captures: Override(0xF), Suspend(6) at boot, Enter(2) at the multiplayer
// menu, Leave after the UDS Shutdown. All results 0 (as before), now with the real counters behind them.
static void TestNdmFfeSequence() {
    std::printf("ndm:u: the FFE sequence\n");
    using namespace ndmtest;
    NdmHle ndm;
    ndm.SetSelfPid(0x77);
    Client c{ndm, ndm.OpenSession(), {}};
    CHECK_EQ(c.s, 0);
    CHECK_EQ(c.Defaults(), 0x09u);
    u32 total = 9;
    // Boss and Nim are suspended once by the default mask 0x09 (Cec | Friend)
    CHECK_EQ(c.DaemonCount(1, &total), 0u);
    CHECK_EQ(total, 1u);
    CHECK_EQ(c.DaemonCount(2, &total), 0u);
    CHECK_EQ(total, 1u);
    CHECK_EQ(c.Status(1), 3u);
    CHECK_EQ(c.Status(0), 1u);

    CHECK_EQ(c.Override(0xF), 0u);
    CHECK_EQ(c.cmd[0], 0x00140040u);
    CHECK_EQ(c.Defaults(), 0x0Fu);
    CHECK_EQ(ndm.daemon_global_count(1), 0u); // the override resumed Boss and Nim
    CHECK_EQ(ndm.daemon_global_count(2), 0u);
    CHECK_EQ(c.Suspend(6), 0u);
    CHECK_EQ(c.cmd[0], 0x00060040u);
    CHECK_EQ(c.DaemonCount(1, &total), 1u);
    CHECK_EQ(total, 1u);
    CHECK_EQ(c.DaemonCount(2, &total), 1u);
    CHECK_EQ(total, 1u);
    CHECK_EQ(c.DaemonCount(0, &total), 0u);
    CHECK_EQ(total, 0u);

    // UDS starts
    CHECK_EQ(c.Enter(2), 0u);
    CHECK_EQ(c.cmd[0], 0x00010040u);
    CHECK_EQ(c.Query(), 2u);
    CHECK_EQ(c.cmd[0], 0x00030080u);
    CHECK_EQ(c.DaemonCount(0, &total), 0u); // Cec: the exclusive state suspends it
    CHECK_EQ(total, 1u);
    CHECK_EQ(c.DaemonCount(1, &total), 1u); // Boss: this session and the exclusive state
    CHECK_EQ(total, 2u);
    CHECK_EQ(c.DaemonCount(3, &total), 0u);
    CHECK_EQ(total, 1u);
    CHECK_EQ(c.SchedCount(&total), 0u);
    CHECK_EQ(total, 1u);
    CHECK_EQ(c.Status(3), 3u);
    CHECK_EQ(c.Enter(2), NdmHle::kResAlreadyInState); // a second Enter by the owner

    // UDS Shutdown, then Leave
    CHECK_EQ(c.Leave(), 0u);
    CHECK_EQ(c.cmd[0], 0x00020040u);
    CHECK_EQ(c.Query(), 0u);
    CHECK_EQ(c.DaemonCount(0, &total), 0u);
    CHECK_EQ(total, 0u);
    CHECK_EQ(c.DaemonCount(1, &total), 1u);
    CHECK_EQ(total, 1u);
    CHECK_EQ(c.SchedCount(&total), 0u);
    CHECK_EQ(total, 0u);
    CHECK_EQ(c.Leave(), 0u); // Leave with nobody owning the state is a success that does nothing

    // the process ends: the session releases its suspends and undoes the override, so Boss and Nim are
    // suspended by the default mask again
    ndm.CloseSession(c.s);
    CHECK_EQ(ndm.default_daemons(), 0x09u);
    CHECK_EQ(ndm.daemon_global_count(1), 1u);
    CHECK_EQ(ndm.daemon_global_count(2), 1u);
    CHECK_EQ(ndm.daemon_global_count(0), 0u);
    CHECK_EQ(ndm.daemon_global_count(3), 0u);
    CHECK_EQ(ndm.header_anomalies(), 0u);
}

static void TestNdmExclusiveState() {
    std::printf("ndm:u: exclusive state, owner, state lock\n");
    using namespace ndmtest;
    NdmHle ndm;
    ndm.SetSelfPid(10);
    Client a{ndm, ndm.OpenSession(), {}};
    Client b{ndm, ndm.OpenSession(), {}};
    // a mode word of 0 is refused before anything else; the state is unchanged
    CHECK_EQ(a.Enter(0), NdmHle::kResInvalidArgument);
    CHECK_EQ(a.Query(), 0u);
    CHECK_EQ(ndm.header_anomalies(), 0u);
    // modes 1, 3, 4: which daemons each one suspends
    CHECK_EQ(a.Enter(1, 5), 0u);
    CHECK_EQ(ndm.daemon_global_count(0), 1u);
    CHECK_EQ(a.Leave(5), 0u);
    CHECK_EQ(ndm.daemon_global_count(0), 0u);
    CHECK_EQ(a.Enter(3, 5), 0u);
    CHECK_EQ(a.Query(), 3u);
    CHECK_EQ(ndm.daemon_global_count(0), 0u);
    CHECK_EQ(ndm.daemon_global_count(1), 2u);
    CHECK_EQ(ndm.daemon_global_count(3), 1u);
    CHECK_EQ(a.Leave(5), 0u);
    CHECK_EQ(ndm.daemon_global_count(1), 1u);
    CHECK_EQ(ndm.daemon_global_count(3), 0u);
    CHECK_EQ(a.Enter(4, 5), 0u);
    CHECK_EQ(a.Query(), 4u);
    CHECK_EQ(a.Leave(5), 0u);
    // only the low byte of the mode goes on: 0x102 is mode 2, 0x100 is a Leave
    CHECK_EQ(a.Enter(0x102, 5), 0u);
    CHECK_EQ(a.Query(), 2u);
    CHECK_EQ(a.Enter(0x100, 5), 0u);
    CHECK_EQ(a.Query(), 0u);
    // modes of 5 and above are state values too (the real service accepts them)
    CHECK_EQ(a.Enter(5, 5), 0u);
    CHECK_EQ(a.Query(), 5u);
    CHECK_EQ(a.Leave(5), 0u);
    CHECK_EQ(a.Query(), 0u);
    // owner semantics with two PIDs
    CHECK_EQ(a.Enter(2, 5), 0u);
    CHECK_EQ(ndm.exclusive_owner(), 5u);
    CHECK_EQ(b.Leave(6), NdmHle::kResNotOwner);    // nobody else can leave it
    CHECK_EQ(b.Enter(2, 6), NdmHle::kResBadState); // from local communications only "none" is reachable
    CHECK_EQ(b.Enter(1, 6), NdmHle::kResBadState);
    CHECK_EQ(a.Query(), 2u);
    CHECK_EQ(ndm.exclusive_owner(), 5u);
    CHECK_EQ(a.Leave(5), 0u);
    CHECK_EQ(a.Query(), 0u);
    CHECK_EQ(ndm.exclusive_owner(), 0u);
    // another PID may take over a state that is not local communications
    CHECK_EQ(a.Enter(1, 5), 0u);
    CHECK_EQ(b.Enter(3, 6), 0u);
    CHECK_EQ(ndm.exclusive_owner(), 6u);
    CHECK_EQ(a.Leave(5), NdmHle::kResNotOwner);
    CHECK_EQ(b.Leave(6), 0u);
    // the lock: recursive per PID, blocks the other PIDs, the owner may still change the state
    CHECK_EQ(a.Unlock(5), NdmHle::kResNotLocked);
    CHECK_EQ(a.Lock(5), 0u);
    CHECK_EQ(a.Lock(5), 0u);
    CHECK_EQ(b.Lock(6), NdmHle::kResLockedByOther);
    CHECK_EQ(b.Enter(1, 6), NdmHle::kResLockedByOther);
    CHECK_EQ(b.Leave(6), NdmHle::kResLockedByOther);
    CHECK_EQ(b.Unlock(6), NdmHle::kResLockedByOther);
    CHECK_EQ(a.Enter(1, 5), 0u);
    CHECK_EQ(a.Leave(5), 0u);
    CHECK_EQ(a.Unlock(5), 0u);
    CHECK_EQ(b.Lock(6), NdmHle::kResLockedByOther); // one level is still held
    CHECK_EQ(a.Unlock(5), 0u);
    CHECK_EQ(b.Lock(6), 0u);
    CHECK_EQ(b.Unlock(6), 0u);
    // closing a session whose PID owns the state leaves it and drops the lock; a session that never
    // named a PID changes nothing
    CHECK_EQ(a.Enter(2, 5), 0u);
    CHECK_EQ(a.Lock(5), 0u);
    Client idle{ndm, ndm.OpenSession(), {}};
    ndm.CloseSession(idle.s);
    CHECK_EQ(b.Query(), 2u);
    ndm.CloseSession(a.s);
    CHECK_EQ(b.Query(), 0u);
    CHECK_EQ(ndm.daemon_global_count(0), 0u);
    CHECK_EQ(b.Lock(6), 0u);
}

static void TestNdmHeaders() {
    std::printf("ndm:u: headers that the firmware refuses are counted and served\n");
    using namespace ndmtest;
    NdmHle ndm;
    ndm.SetSelfPid(10);
    Client a{ndm, ndm.OpenSession(), {}};
    // wrong header (normal count), wrong PID descriptor: the standard layout is used all the same
    CHECK_EQ(a.Call(H(1, 1, 0), 2, 0x20, 5), 0u);
    CHECK_EQ(a.cmd[0], 0x00010040u);
    CHECK_EQ(ndm.header_anomalies(), 1u);
    CHECK_EQ(a.Query(), 2u);
    CHECK_EQ(a.Leave(5), 0u);
    CHECK_EQ(a.Call(H(1, 1, 2), 2, 0x00, 5), 0u);
    CHECK_EQ(ndm.header_anomalies(), 2u);
    CHECK_EQ(a.Call(H(2, 1, 2), 0x20, 5), 0u);
    CHECK_EQ(ndm.header_anomalies(), 3u);
    CHECK_EQ(a.Call(H(4, 1, 2), 0x20, 5), 0u);
    CHECK_EQ(a.Call(H(5, 1, 2), 0x20, 5), 0u);
    CHECK_EQ(ndm.header_anomalies(), 5u);
    CHECK_EQ(a.Call(H(0x18, 0, 4), 0x20, 0, 1, 0x1234), 0u); // the copy-handle descriptor must be 0
    CHECK_EQ(ndm.header_anomalies(), 6u);
    // the right ones are not counted, nor are the commands the firmware does not validate
    CHECK_EQ(a.Enter(2, 5), 0u);
    CHECK_EQ(a.Leave(5), 0u);
    CHECK_EQ(a.Lock(5), 0u);
    CHECK_EQ(a.Unlock(5), 0u);
    CHECK_EQ(a.Call(H(0x18, 0, 4), 0x20, 0, 0, 0x1234), 0u);
    CHECK_EQ(a.Suspend(0x1234), NdmHle::kResInvalidArgument);
    CHECK_EQ(a.Query(), 0u);
    CHECK_EQ(ndm.header_anomalies(), 6u);
    // a request on a session that is not open gets the dispatcher's own answer
    ndm.CloseSession(a.s);
    CHECK_EQ(a.Call(H(3, 0, 0)), NdmHle::kResBadHeader);
    CHECK_EQ(a.cmd[0], 0x00000040u);
    CHECK_EQ(ndm.session_open(a.s), false);
    u32 cmd[4] = {H(3, 0, 0), 0, 0, 0};
    ndm.Dispatch(-1, cmd);
    CHECK_EQ(cmd[1], NdmHle::kResBadHeader);
    ndm.Dispatch(NdmHle::kMaxSessions, cmd);
    CHECK_EQ(cmd[1], NdmHle::kResBadHeader);
}

// The PID of a request is the kernel's to fill in; the plugin serves the request before the kernel does.
static void TestNdmPidWords() {
    std::printf("ndm:u: PID words that the kernel never filled in\n");
    using namespace ndmtest;
    {
        NdmHle ndm;
        ndm.SetSelfPid(0x42, false); // what the plugin does
        Client a{ndm, ndm.OpenSession(), {}};
        Client b{ndm, ndm.OpenSession(), {}};
        // libctru leaves whatever the buffer held in the word after the descriptor
        CHECK_EQ(a.Enter(2, 0x11111111), 0u);
        CHECK_EQ(ndm.exclusive_owner(), 0x42u);
        CHECK_EQ(a.Enter(2, 0x22222222), NdmHle::kResAlreadyInState); // the same process, whatever the word says
        CHECK_EQ(b.Leave(0x33333333), 0u);                            // and so is the other session's
        CHECK_EQ(a.Query(), 0u);
        CHECK_EQ(ndm.exclusive_owner(), 0u);
        CHECK_EQ(a.Lock(0xAAAAAAAA), 0u);
        CHECK_EQ(b.Lock(0xBBBBBBBB), 0u); // one process: the lock is recursive across its sessions
        CHECK_EQ(a.Unlock(0xCCCCCCCC), 0u);
        CHECK_EQ(b.Unlock(0xDDDDDDDD), 0u);
        CHECK_EQ(a.Unlock(0xEEEEEEEE), NdmHle::kResNotLocked);
        CHECK_EQ(ndm.header_anomalies(), 0u);
    }
    {
        NdmHle ndm;
        ndm.SetSelfPid(0, false); // 0 is the real state's "nobody": the process gets a non-zero stand-in
        Client a{ndm, ndm.OpenSession(), {}};
        CHECK_EQ(a.Enter(2), 0u);
        CHECK(ndm.exclusive_owner() != 0u);
        CHECK_EQ(a.Leave(), 0u);
    }
    {
        NdmHle ndm; // a caller behind a kernel: the word is the PID, 0 stands for this process
        ndm.SetSelfPid(0x42);
        Client a{ndm, ndm.OpenSession(), {}};
        CHECK_EQ(a.Enter(2, 0), 0u);
        CHECK_EQ(ndm.exclusive_owner(), 0x42u);
        CHECK_EQ(a.Leave(0x43), NdmHle::kResNotOwner);
        CHECK_EQ(a.Leave(0x42), 0u);
    }
}

static void TestNdmDaemonsAndScheduler() {
    std::printf("ndm:u: daemons, scheduler, intervals, override\n");
    using namespace ndmtest;
    NdmHle ndm;
    Client a{ndm, ndm.OpenSession(), {}};
    Client b{ndm, ndm.OpenSession(), {}};
    u32 t;
    // mask validation
    CHECK_EQ(a.Suspend(0), NdmHle::kResInvalidArgument);
    CHECK_EQ(a.Suspend(16), NdmHle::kResInvalidArgument);
    CHECK_EQ(a.Resume(0), NdmHle::kResInvalidArgument);
    CHECK_EQ(a.Resume(0x100), NdmHle::kResInvalidArgument);
    // resuming what this session did not suspend changes nothing at all, not even the valid bits
    CHECK_EQ(a.Suspend(1), 0u);
    CHECK_EQ(a.Resume(1 | 8), NdmHle::kResNotSuspended);
    CHECK_EQ(a.DaemonCount(0, &t), 1u);
    CHECK_EQ(a.Resume(1), 0u);
    CHECK_EQ(a.Resume(1), NdmHle::kResNotSuspended);
    // the global count counts sessions, not calls
    CHECK_EQ(a.Suspend(8), 0u);
    CHECK_EQ(a.Suspend(8), 0u);
    CHECK_EQ(b.Suspend(8), 0u);
    CHECK_EQ(a.DaemonCount(3, &t), 2u);
    CHECK_EQ(t, 2u);
    CHECK_EQ(a.Resume(8), 0u);
    CHECK_EQ(ndm.daemon_global_count(3), 2u);
    CHECK_EQ(a.Resume(8), 0u);
    CHECK_EQ(ndm.daemon_global_count(3), 1u);
    CHECK_EQ(b.Resume(8), 0u);
    CHECK_EQ(ndm.daemon_global_count(3), 0u);
    // a cap of 100 per daemon and session, checked before anything changes
    for (int i = 0; i < 100; ++i) {
        CHECK_EQ(a.Suspend(4), 0u);
    }
    CHECK_EQ(a.Suspend(4), NdmHle::kResBadState);
    CHECK_EQ(a.Suspend(4 | 8), NdmHle::kResBadState);
    CHECK_EQ(a.DaemonCount(3, &t), 0u);
    // an invalid daemon
    CHECK_EQ(a.Call(H(0xD, 1, 0), 4), NdmHle::kResInvalidArgument);
    CHECK_EQ(a.cmd[0], 0x000D0080u);
    CHECK_EQ(a.cmd[2], 0u);
    CHECK_EQ(a.Call(H(0xE, 1, 0), 9), NdmHle::kResInvalidArgument);
    CHECK_EQ(a.cmd[0], 0x000E00C0u);
    CHECK_EQ(a.Call(H(0xD, 1, 0), 0x100), 0u); // (the low byte only: daemon 0)
    CHECK_EQ(a.cmd[2], 1u);

    // the scheduler
    CHECK_EQ(a.ResumeSched(), NdmHle::kResNotSuspended);
    CHECK_EQ(a.SuspendSched(1), 0u);
    CHECK_EQ(a.SchedCount(&t), 1u);
    CHECK_EQ(t, 1u);
    CHECK_EQ(a.cmd[0], 0x000F00C0u);
    CHECK_EQ(ndm.daemon_global_count(0), 1u); // it suspends every daemon
    CHECK_EQ(a.SuspendSched(0), 0u);
    CHECK_EQ(a.SchedCount(&t), 2u);
    CHECK_EQ(t, 1u); // the global count is per session
    CHECK_EQ(a.ResumeSched(), 0u);
    CHECK_EQ(a.ResumeSched(), 0u);
    CHECK_EQ(a.SchedCount(&t), 0u);
    CHECK_EQ(t, 0u);
    CHECK_EQ(ndm.daemon_global_count(0), 0u);
    for (int i = 0; i < 100; ++i) {
        CHECK_EQ(a.SuspendSched(), 0u);
    }
    CHECK_EQ(a.SuspendSched(), NdmHle::kResBadState);
    // closing the session resumes what it suspended, however often it did
    ndm.CloseSession(a.s);
    CHECK_EQ(ndm.scheduler_global_count(), 0);
    CHECK_EQ(ndm.daemon_global_count(0), 0u);
    CHECK_EQ(ndm.daemon_global_count(1), 1u); // (the default mask's)
    CHECK_EQ(ndm.daemon_global_count(2), 1u);
    CHECK_EQ(ndm.daemon_global_count(3), 0u);
    Client a2{ndm, ndm.OpenSession(), {}};
    CHECK_EQ(a2.s, a.s);
    CHECK_EQ(a2.SchedCount(&t), 0u); // the session object starts empty

    // the intervals: global, put back when the session that changed them closes
    CHECK_EQ(a2.Call(H(0x11, 0, 0)), 0u);
    CHECK_EQ(a2.cmd[2], 30u);
    CHECK_EQ(a2.cmd[0], 0x00110080u);
    CHECK_EQ(a2.Call(H(0x13, 0, 0)), 0u);
    CHECK_EQ(a2.cmd[2], 10u);
    CHECK_EQ(b.Call(H(0x10, 1, 0), 99), 0u);
    CHECK_EQ(b.cmd[0], 0x00100040u);
    CHECK_EQ(b.Call(H(0x12, 1, 0), 0), 0u); // no validation
    CHECK_EQ(a2.Call(H(0x11, 0, 0)), 0u);
    CHECK_EQ(a2.cmd[2], 99u);
    CHECK_EQ(a2.Call(H(0x13, 0, 0)), 0u);
    CHECK_EQ(a2.cmd[2], 0u);
    ndm.CloseSession(b.s);
    CHECK_EQ(ndm.scan_interval(), 30u);
    CHECK_EQ(ndm.retry_interval(), 10u);

    // the override: one owner at a time, the low byte is kept, only the owner resets it
    Client c{ndm, ndm.OpenSession(), {}};
    CHECK_EQ(c.Reset(), NdmHle::kResBadState);
    CHECK_EQ(c.Override(0x0F), 0u);
    CHECK_EQ(a2.Override(0x01), NdmHle::kResOverrideTaken);
    CHECK_EQ(a2.Reset(), NdmHle::kResBadState);
    CHECK_EQ(c.Override(0x1F3), 0u); // the owner may override again
    CHECK_EQ(c.Defaults(), 0xF3u);
    CHECK_EQ(c.Reset(), 0u);
    CHECK_EQ(c.Defaults(), 0x09u);
    CHECK_EQ(c.Reset(), NdmHle::kResBadState);
    CHECK_EQ(a2.Override(0x01), 0u);
    CHECK_EQ(a2.Defaults(), 0x01u);
    ndm.CloseSession(a2.s); // the owner closes: the default is back
    CHECK_EQ(ndm.default_daemons(), 0x09u);
    CHECK_EQ(c.Override(0x02), 0u);
    CHECK_EQ(ndm.daemon_global_count(0), 1u); // Cec lost its default bit
    CHECK_EQ(ndm.daemon_global_count(1), 0u); // Boss gained it
    CHECK_EQ(ndm.daemon_global_count(3), 1u);
    CHECK_EQ(c.Reset(), 0u);
    CHECK_EQ(ndm.daemon_global_count(0), 0u);
    CHECK_EQ(ndm.daemon_global_count(1), 1u);
    CHECK_EQ(ndm.daemon_global_count(3), 0u);

    // 0x0C: an error and a null handle; 0x17: success; 0x18 needs an event
    c.Call(H(0xC, 0, 0));
    CHECK_EQ(c.cmd[0], 0x000C0042u);
    CHECK_EQ(c.cmd[1], 0xE0C06BF4u);
    CHECK_EQ(c.cmd[2], 0u);
    CHECK_EQ(c.cmd[3], 0u);
    CHECK_EQ(c.Call(H(0x17, 0, 0)), 0u);
    CHECK_EQ(c.cmd[0], 0x00170040u);
    CHECK_EQ(c.Call(H(0x18, 0, 4), 0x20, 0, 0, 0x1234), 0u);
    CHECK_EQ(c.cmd[0], 0x00180040u);
    CHECK_EQ(c.Call(H(0x18, 0, 4), 0x20, 0, 0, 0), 0xC8A06809u);
    // GetCurrentState / GetTargetState are not known to be anything else than what they always answered
    for (u32 id : {0xAu, 0xBu}) {
        c.Call(H(id, 0, 0));
        CHECK_EQ(c.cmd[0], (id << 16) | 0x80u);
        CHECK_EQ(c.cmd[1], 0u);
        CHECK_EQ(c.cmd[2], 0u);
    }
    // ids the firmware does not know (until the New 3DS ndm is seen): success, and not an anomaly
    CHECK_EQ(c.Call(H(0x19, 0, 0)), 0u);
    CHECK_EQ(c.cmd[0], 0x00190040u);
    CHECK_EQ(c.Call(H(0xFF, 0, 0)), 0u);
    CHECK_EQ(c.Call(H(0, 0, 0)), 0u);
    CHECK_EQ(ndm.header_anomalies(), 0u);
}

static void TestNdmSessions() {
    std::printf("ndm:u: sessions\n");
    NdmHle ndm;
    CHECK_EQ(NdmHle::kMaxSessions, 6); // what the real service registers
    int s[NdmHle::kMaxSessions];
    for (int i = 0; i < NdmHle::kMaxSessions; ++i) {
        s[i] = ndm.OpenSession();
        CHECK_EQ(s[i], i);
    }
    CHECK_EQ(ndm.OpenSession(), -1); // the real service has 6
    ndm.CloseSession(s[2]);
    CHECK_EQ(ndm.session_open(s[2]), false);
    CHECK_EQ(ndm.OpenSession(), s[2]);
    ndm.CloseSession(s[2]);
    ndm.CloseSession(s[2]); // closing twice, or a session that does not exist, changes nothing
    ndm.CloseSession(-1);
    ndm.CloseSession(NdmHle::kMaxSessions);
    CHECK_EQ(ndm.default_daemons(), 0x09u);
    CHECK_EQ(ndm.daemon_global_count(1), 1u);
}

// ---------------------------------------------------------------------------------------------
// Azahar-peer rule checker (azahar_rules.hpp): it must catch every rule it claims and stay quiet on
// a legitimate join. A checker that never fires would make CHECK_RULES() in the scenarios worthless.
// ---------------------------------------------------------------------------------------------
static WifiPacket MakePkt(WifiPacket::Type type, const Mac& from, const Mac& to, std::vector<u8> data) {
    WifiPacket p;
    p.type = type;
    p.channel = 11;
    p.transmitter_address = from;
    p.destination_address = to;
    p.data = std::move(data);
    return p;
}

static void TestAzaharRules() {
    std::printf("azahar-peer rule checker\n");
    const Mac host{0, 0x1F, 0x32, 0, 0, 1}, a{0, 0x1F, 0x32, 0, 0, 2}, b{0, 0x1F, 0x32, 0, 0, 3};
    const NodeInfo node = MakeNode(0x1234, "Rule", 0);
    std::vector<NodeInfo> nodes(4);
    nodes[0] = MakeNode(1, "Host", 1);
    auto auth = [&](const Mac& from, const Mac& to, AuthSeq seq) {
        return MakePkt(WifiPacket::Type::Authentication, from, to, GenerateAuthenticationFrame(seq));
    };
    auto assoc = [&](const Mac& from, const Mac& to, u16 status) {
        return MakePkt(WifiPacket::Type::AssociationResponse, from, to,
                       GenerateAssocResponseFrame(status, 1, 0xDEADBEEF));
    };
    auto start = [&](const Mac& from, const Mac& to) {
        return MakePkt(WifiPacket::Type::Data, from, to, GenerateEapolStartFrame(1, ConnectionType::Client, node));
    };
    auto logoff = [&](const Mac& from, const Mac& to, u8 total, u8 max) {
        return MakePkt(WifiPacket::Type::Data, from, to, GenerateEapolLogoffFrame(2, nodes, max, total));
    };
    auto secure = [&](const Mac& from, const Mac& to, u16 dest, u16 src, u16 seq) {
        const u8 payload[] = {1, 2, 3};
        return MakePkt(WifiPacket::Type::Data, from, to, GenerateDataPayload(payload, 3, 7, dest, src, seq));
    };
    auto deauth = [&](const Mac& from, const Mac& to) {
        return MakePkt(WifiPacket::Type::Deauthentication, from, to, {});
    };

    { // a legitimate join, some data and a leave: quiet
        AzaharRules r;
        r.AddMember(host);
        r.AddMember(a);
        r.Inspect(auth(a, host, AuthSeq::Seq1));
        r.Inspect(auth(host, a, AuthSeq::Seq2));
        r.Inspect(assoc(host, a, 0));
        r.Inspect(start(a, host));
        r.Inspect(logoff(host, kBroadcastMac, 2, 4));
        r.Inspect(secure(a, host, 1, 2, 0));
        r.Inspect(secure(host, kBroadcastMac, 2, 1, 0));
        r.Inspect(deauth(a, host));
        CHECK_EQ(r.count(), std::size_t(0));
        CHECK_EQ(r.inspected(), std::size_t(8));
    }
    { // nwm_uds.cpp:394
        AzaharRules r;
        WifiPacket p = secure(a, host, 1, 2, 0);
        p.data[kLlcHeaderSize + 6] = 1; // is_management
        r.Inspect(p);
        CHECK(r.Has("secure-management"));
        CHECK_EQ(r.count(), std::size_t(1));
    }
    { // nwm_uds.cpp:177
        AzaharRules r;
        r.Inspect(assoc(host, a, 17));
        CHECK(r.Has("assoc-status"));
        AzaharRules ok;
        ok.Inspect(assoc(host, a, 0));
        CHECK_EQ(ok.count(), std::size_t(0));
    }
    { // nwm_uds.cpp:224: a Start to a host that is full
        AzaharRules r;
        r.Inspect(start(a, host));
        r.Inspect(logoff(host, kBroadcastMac, 2, 2)); // 2 of 2: full
        CHECK_EQ(r.count(), std::size_t(0));
        r.Inspect(start(b, host));
        CHECK(r.Has("start-to-full"));
        // ... once the first client has left there is room again
        AzaharRules room_again;
        room_again.Inspect(start(a, host));
        room_again.Inspect(logoff(host, kBroadcastMac, 2, 2));
        room_again.Inspect(deauth(a, host));
        room_again.Inspect(start(b, host));
        CHECK_EQ(room_again.count(), std::size_t(0));
    }
    { // a Logoff broadcast answers the first Start of a client, nothing else
        AzaharRules r;
        r.Inspect(logoff(host, kBroadcastMac, 2, 4)); // nobody asked
        CHECK(r.Has("logoff-broadcast"));
        AzaharRules again;
        again.Inspect(start(a, host));
        again.Inspect(logoff(host, kBroadcastMac, 2, 4)); // first admission
        again.Inspect(start(a, host));                    // the client repeats its Start
        again.Inspect(logoff(host, a, 2, 4));             // unicast answer
        CHECK_EQ(again.count(), std::size_t(0));
        again.Inspect(start(a, host));
        again.Inspect(logoff(host, kBroadcastMac, 2, 4)); // a Connecting client would adopt it
        CHECK(again.Has("logoff-broadcast"));
    }
    { // Azahar looks at the repeated Start of an admitted client before it asserts on the count
        AzaharRules r;
        r.Inspect(start(a, host));
        r.Inspect(logoff(host, kBroadcastMac, 2, 2));
        r.Inspect(start(a, host));
        r.Inspect(logoff(host, a, 2, 2));
        CHECK_EQ(r.count(), std::size_t(0));
        r.Inspect(start(b, host));
        CHECK(r.Has("start-to-full"));
    }
    { // a client that authenticates again starts a new association: its next Start is a first admission
        AzaharRules r;
        r.Inspect(start(a, host));
        r.Inspect(logoff(host, kBroadcastMac, 2, 4));
        r.Inspect(auth(a, host, AuthSeq::Seq1));
        r.Inspect(start(a, host));
        r.Inspect(logoff(host, kBroadcastMac, 2, 4));
        CHECK_EQ(r.count(), std::size_t(0));
    }
    { // Deauthentication only to the broadcast MAC or to somebody the room knows
        AzaharRules r;
        r.AddMember(host);
        r.Inspect(deauth(host, kBroadcastMac));
        CHECK_EQ(r.count(), std::size_t(0));
        r.Inspect(deauth(host, a)); // a has never been heard from
        CHECK(r.Has("deauth-unknown"));
        r.Inspect(deauth(host, Mac{}));
        CHECK_EQ(r.count(), std::size_t(2));
        AzaharRules heard;
        heard.Inspect(secure(a, host, 1, 2, 0)); // a speaks: now it is known
        heard.Inspect(deauth(host, a));
        CHECK_EQ(heard.count(), std::size_t(0));
    }
    { // SecureData the way Azahar takes it (a numbered frame is fine: the plugin numbers its own)
        AzaharRules r;
        r.Inspect(secure(a, host, 1, 2, 7));
        CHECK(!r.Has("secure-shape")); // the sequence number is free
        AzaharRules c;
        WifiPacket p = secure(a, host, 1, 2, 0);
        p.data[kLlcHeaderSize + 2] = 1; // aggregate count
        c.Inspect(p);
        CHECK(c.Has("secure-shape"));
        AzaharRules s;
        p = secure(a, host, 1, 2, 0);
        p.data[kLlcHeaderSize + 5] += 1; // securedata_size no longer agrees with protocol_size
        s.Inspect(p);
        CHECK(s.Has("secure-shape"));
    }
}

// The Azahar host model is test infrastructure: check that it behaves like Azahar's host next to
// real engine clients (re-broadcast of client-to-client frames, silent leave) and keeps the rules.
static void TestAzaharHostModel() {
    std::printf("azahar host model (+ 2 engine clients)\n");
    g_rules.Reset();
    MockRoom room;
    NetworkInfo ni = MakeNetworkInfo(4, 6);
    AzaharHost host(room, 1, ni, MakeNode(0x1111, "AzHost", 0));
    Console alice(room, 2), bob(room, 3);
    u32 ev, bind_ev;
    CHECK_EQ(alice.engine->Initialize(MakeNode(0x2222, "Alice", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK_EQ(bob.engine->Initialize(MakeNode(0x3333, "Bob", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
    CHECK_EQ(alice.engine->Bind(2, 0x5F4, 1, kBroadcastNetworkNodeId, &bind_ev), 0u);
    CHECK_EQ(bob.engine->Bind(2, 0x5F4, 1, kBroadcastNetworkNodeId, &bind_ev), 0u);

    host.SendBeacon();
    room.WaitIdle();
    std::vector<u8> scan(0x4000);
    CHECK_EQ(alice.engine->RecvBeaconBroadcastData(scan.data(), static_cast<u32>(scan.size()), kBroadcastMac), 0u);
    NetworkInfo scanned;
    CHECK(FirstScannedNetwork(scan, &scanned));
    CHECK_EQ(Hex(scanned.host_mac_address.data(), 6), Hex(host.mac().data(), 6));
    CHECK_EQ(scanned.max_nodes, 4);
    CHECK_EQ(scanned.application_data_size, 6);
    CHECK_EQ(scanned.wlan_comm_id(), 0x0010CAFEu);
    CHECK_EQ(scanned.channel, kDefaultNetworkChannel);

    CHECK_EQ(alice.engine->ConnectToNetwork(reinterpret_cast<u8*>(&scanned), sizeof(scanned), 1), 0u);
    room.WaitIdle(); // (the Logoff that admitted alice is still on its way to bob, who must not take it for his)
    CHECK_EQ(bob.engine->ConnectToNetwork(reinterpret_cast<u8*>(&scanned), sizeof(scanned), 1), 0u);
    room.WaitIdle();
    CHECK_EQ(host.admissions(), 2u);
    CHECK_EQ(host.total(), 3);
    CHECK_EQ(alice.engine->GetConnectionStatus().network_node_id, 2);
    CHECK_EQ(bob.engine->GetConnectionStatus().network_node_id, 3);
    CHECK_EQ(alice.engine->GetConnectionStatus().total_nodes, 3);

    u16 src = 0;
    u32 size = 0;
    auto copies_for_bob = [&] {
        int copies = 0;
        for (;;) {
            PullAll(bob, 2, &src, &size);
            if (size == 0) {
                return copies;
            }
            CHECK_EQ(src, 2);
            ++copies;
        }
    };
    // alice -> bob with the broadcast flag (what FFE does): the room hands bob the direct copy and
    // the Azahar host re-broadcasts the frame, whatever the MAC was.
    const u8 msg[] = "broadcast flag";
    CHECK_EQ(alice.engine->SendTo(3, 1, sizeof(msg), 2, msg, sizeof(msg)), 0u);
    room.WaitIdle();
    const int copies = copies_for_bob();
    CHECK(copies >= 1 && copies <= 2); // 2 today; 1 once the client drops the host's twin
    CHECK_EQ(host.forwarded(), 1u);
    CHECK_EQ(host.TakeInbox().size(), std::size_t(0));

    // alice -> host: the host MAC is the destination, the host keeps the frame
    CHECK_EQ(alice.engine->SendTo(1, 1, sizeof(msg), 0, msg, sizeof(msg)), 0u);
    room.WaitIdle();
    auto inbox = host.TakeInbox();
    CHECK_EQ(inbox.size(), std::size_t(1));
    if (inbox.size() == 1) {
        CHECK_EQ(inbox[0].src, 2);
        CHECK_EQ(inbox[0].dest, 1);
        CHECK(inbox[0].payload.size() == sizeof(msg) && std::memcmp(inbox[0].payload.data(), msg, sizeof(msg)) == 0);
    }
    CHECK_EQ(host.forwarded(), 1u);

    // alice -> bob without the flag: bob's own MAC, the host never sees it
    CHECK_EQ(alice.engine->SendTo(3, 1, sizeof(msg), 0, msg, sizeof(msg)), 0u);
    room.WaitIdle();
    CHECK_EQ(copies_for_bob(), 1);
    CHECK_EQ(host.forwarded(), 1u);
    CHECK_EQ(host.TakeInbox().size(), std::size_t(0));

    // host -> alice with the broadcast flag: alice delivers it once, bob drops it as not for him
    CHECK(host.SendData(2, 1, 2, std::vector<u8>(msg, msg + sizeof(msg))));
    room.WaitIdle();
    PullAll(alice, 2, &src, &size);
    CHECK_EQ(size, u32(sizeof(msg)));
    CHECK_EQ(src, 1);
    PullAll(alice, 2, &src, &size);
    CHECK_EQ(size, 0u);
    CHECK_EQ(copies_for_bob(), 0);

    // alice leaves with a Deauthentication; the Azahar host drops her without telling anybody
    CHECK_EQ(alice.engine->DisconnectNetwork(), 0u);
    room.WaitIdle();
    CHECK_EQ(host.departures(), 1u);
    CHECK_EQ(host.total(), 2);
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------
// Debug capture (spec C14): the lines AzaharLink.debug.log is made of
// ---------------------------------------------------------------------------------------------
// Pops one record and formats it as the plugin does, with "now" as the start so the time reads 0.
static std::string PopLine() {
    DebugRecord rec;
    u8 buf[1700];
    if (!DebugPop(&rec, buf, sizeof(buf))) {
        return "<ring empty>";
    }
    std::string out;
    DebugFormat(rec, buf, rec.ms, &out);
    return out;
}

static std::vector<std::string> SplitWords(const std::string& s) {
    std::vector<std::string> words;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n')) {
            ++i;
        }
        std::size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\n') {
            ++j;
        }
        if (j > i) {
            words.push_back(s.substr(i, j - i));
        }
        i = j;
    }
    return words;
}

static void TestDebugCapture() {
    std::printf("debug capture lines\n");
    CHECK(DebugInit());
    g_debug_level = 1;

    // Initialize as FFE sends it: 14 request words, 3 reply words. Every word is there.
    const u32 init_req[14] = {0x00014000, 0x0a5cc011, 0x37110002, 0x00690047, 0x006e006f, 0x006f006b, 0,
                              0,          0,          0,          0,          0x00000400, 0,          0x00130003};
    const u32 init_rep[3] = {0, 0, 0x0042005c};
    DebugCommand("uds cmd 0x1b", init_req, 14, init_rep, 3);
    CHECK_EQ(PopLine(),
             std::string("     0.000 uds cmd 0x1b req 00014000 0a5cc011 37110002 00690047 006e006f 006f006b "
                         "00000000 00000000 00000000 00000000 00000000 00000400 00000000 00130003 "
                         "-> rep 00000000 00000000 0042005c\n"));

    // GetConnectionStatus: no request word at all, thirteen reply words
    u32 status_rep[13] = {0, 9, 1, 0x000f0004};
    DebugCommand("uds cmd 0x0b", nullptr, 0, status_rep, 13);
    const std::string status_line = PopLine();
    CHECK(status_line.find("uds cmd 0x0b req -> rep 00000000 00000009 00000001 000f0004 ") != std::string::npos);
    {
        const auto words = SplitWords(status_line);
        // time, "uds", "cmd", "0x0b", "req", "->", "rep", then the 13 words
        CHECK_EQ(words.size(), std::size_t(7 + 13));
    }

    // The analysis scripts' pattern must still match both lines, any word count
    {
        const std::regex census(R"(^\s*(\d+\.\d+) (uds|ndm) cmd 0x([0-9a-f]+) req ((?:[0-9a-f]{8} ?)*)-> rep ((?:[0-9a-f]{8} ?)*))");
        std::smatch m;
        const std::string line = "     0.000 uds cmd 0x1b req 00014000 0a5cc011 -> rep 00000000 00000000 0042005c\n";
        CHECK(std::regex_search(line, m, census));
        CHECK(m.size() == 6 && m[4].str() == "00014000 0a5cc011 " && m[5].str() == "00000000 00000000 0042005c");
        CHECK(std::regex_search(status_line, m, census));
        CHECK(m.size() == 6 && m[4].str().empty());
    }

    // The PullPacket line of the scripts, with the words appended after it
    {
        const u32 req[3] = {2, 0x172, 0x5C6};
        const u32 rep[5] = {0, 140, 3, 0x0002300a, 0x00810000};
        DebugCommand("uds PullPacket bind 2: 140 bytes from node 3", req, 3, rep, 5);
        const std::string line = PopLine();
        const std::regex pull(R"(^\s*([\d.]+) uds PullPacket bind (\d+): (\d+) bytes from node (\d+))");
        std::smatch m;
        CHECK(std::regex_search(line, m, pull));
        CHECK(m.size() == 5 && m[2].str() == "2" && m[3].str() == "140" && m[4].str() == "3");
        CHECK(line.find(" req 00000002 00000172 000005c6 -> rep 00000000 0000008c 00000003 0002300a 00810000") !=
              std::string::npos);
    }

    // The most any IPC buffer can carry (63 words after the header) is kept, more is cut
    {
        u32 many[200];
        for (u32 i = 0; i < 200; ++i) {
            many[i] = 0x10000000 + i;
        }
        DebugCommand("uds cmd 0x0f", many, 200, many, 200);
        const auto words = SplitWords(PopLine());
        CHECK_EQ(words.size(), std::size_t(7 + 63 + 63)); // time uds cmd 0x0f req -> rep, then 126 words
        CHECK_EQ(words.size() > 5 ? words[5] : std::string(), std::string("10000000"));
        CHECK_EQ(words.back(), std::string("1000003e"));
    }

    // FRAME lines keep their format exactly (scripts: load.js, gaps.js, mkreplay.py)
    {
        const u8 payload[5] = {1, 2, 3, 4, 5};
        DebugFrame(kDirTx, 4, 0xFFFF, 243, payload, 5);
        const std::string line = PopLine();
        CHECK_EQ(line, std::string("     0.000 FRAME tx    4>all ch243 5B: 0102030405\n"));
        const std::regex frame(R"(^\s*([\d.]+) FRAME (\S+)\s+(\S+)>(\S+) ch(\d+) (\d+)B(\s\(cut\))?: ([0-9a-f]*)$)");
        std::smatch m;
        const std::string no_newline = line.substr(0, line.size() - 1);
        CHECK(std::regex_search(no_newline, m, frame));
        CHECK(m.size() == 9 && m[2].str() == "tx" && m[3].str() == "4" && m[4].str() == "all" && m[8].str() == "0102030405");
        std::vector<u8> big(2000, 0xAB);
        DebugFrame(kDirOther, 2, 3, 7, big.data(), static_cast<u32>(big.size()));
        const std::string cut = PopLine();
        CHECK(cut.find("FRAME other 2>3 ch7 2000B (cut): ab") != std::string::npos);
        CHECK_EQ(cut.size(), std::string("     0.000 FRAME other 2>3 ch7 2000B (cut): ").size() + 1600 * 2 + 1);
    }

    // A beacon is kept whole, with the transmitter in front, under a keyword of its own
    {
        std::vector<u8> body(377);
        for (std::size_t i = 0; i < body.size(); ++i) {
            body[i] = static_cast<u8>(i * 7 + 1);
        }
        const Mac tx{0x00, 0x1F, 0x32, 0xAA, 0xBB, 0xCC};
        DebugBeacon(kDirRx, tx.data(), 11, body.data(), static_cast<u32>(body.size()));
        const std::string line = PopLine();
        const std::string head = "     0.000 BEACON rx    00:1f:32:aa:bb:cc ch11 377B: ";
        CHECK_EQ(line.compare(0, head.size(), head), 0);
        CHECK_EQ(line.size(), head.size() + 377 * 2 + 1);
        CHECK_EQ(line.substr(head.size(), 377 * 2), Hex(body.data(), body.size()));
        const std::regex frame(R"(FRAME)");
        CHECK(!std::regex_search(line, frame)); // FRAME readers never see it
    }

    // Nothing is recorded with the capture off, and the ring is empty again
    g_debug_level = 0;
    DebugCommand("uds cmd 0x0b", nullptr, 0, status_rep, 13);
    DebugBeacon(kDirRx, nullptr, 11, nullptr, 0);
    CHECK_EQ(PopLine(), std::string("<ring empty>"));
    CHECK_EQ(DebugTakeDropped(), 0u);
}

// The role the game sees (off / idle / connecting / host / client / spectator) is logged whenever it
// changes, in the main log and in the debug capture.
static void TestRoleLines() {
    std::printf("role lines\n");
    std::vector<std::string> lines;
    std::mutex lines_mutex;
    SetLogSink([&](const char* line) {
        std::lock_guard<std::mutex> l(lines_mutex);
        lines.push_back(line);
    });
    CHECK(DebugInit());
    g_debug_level = 1;
    g_rules.Reset();
    {
        MockRoom room;
        NetworkInfo ni = MakeNetworkInfo(4, 6);
        Console host(room, 1), alice(room, 2);
        u32 ev;
        CHECK_EQ(host.engine->Initialize(MakeNode(0x1111, "Host", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        CHECK_EQ(alice.engine->Initialize(MakeNode(0x2222, "Alice", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        CHECK_EQ(host.engine->BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni)), 0u);
        host.engine->Tick(platform::NowMs() + 100000);
        room.WaitIdle();
        std::vector<u8> scan(0x4000);
        CHECK_EQ(alice.engine->RecvBeaconBroadcastData(scan.data(), static_cast<u32>(scan.size()), kBroadcastMac), 0u);
        NetworkInfo scanned;
        CHECK(FirstScannedNetwork(scan, &scanned));
        CHECK_EQ(alice.engine->ConnectToNetwork(reinterpret_cast<u8*>(&scanned), sizeof(scanned), 1), 0u);
        room.WaitIdle();
        CHECK_EQ(alice.engine->DisconnectNetwork(), 0u);
        CHECK_EQ(host.engine->DestroyNetwork(), 0u);
        room.WaitIdle();
        CHECK_EQ(alice.engine->Shutdown(), 0u);
        CHECK_EQ(host.engine->Shutdown(), 0u);
        CHECK_RULES();
    }
    SetLogSink([](const char*) {});

    auto count = [&](const char* needle) {
        int n = 0;
        for (const std::string& line : lines) {
            if (line.find(needle) != std::string::npos) {
                ++n;
            }
        }
        return n;
    };
    CHECK_EQ(count("uds: role off -> idle "), 2);        // two Initialize
    CHECK_EQ(count("uds: role idle -> host (node 1, nodes 1/4, reason 1)"), 1);
    CHECK_EQ(count("uds: role idle -> connecting "), 1);
    CHECK_EQ(count("uds: role connecting -> client (node 2, nodes 2/4, reason 1)"), 1);
    CHECK_EQ(count("uds: role client -> idle "), 1);      // Disconnect
    CHECK_EQ(count("uds: role host -> idle "), 1);        // Destroy
    CHECK_EQ(count("uds: role idle -> off "), 2);         // two Shutdown
    CHECK_EQ(count("uds: role "), 9);                      // and nothing else
    // The lines are made by hand now (a game's thread can reach them: azl/line.hpp); every one must still be exactly
    // what the printf formats of 0.1.15 made of the same numbers: parsed with the format and printed with it again
    int exact = 0;
    for (const std::string& line : lines) {
        if (line.compare(0, 10, "uds: role ") != 0) {
            continue;
        }
        char from[16], to[16];
        unsigned node = 0, nodes = 0, max_nodes = 0;
        unsigned long reason = 0;
        char again[160];
        if (std::sscanf(line.c_str(), "uds: role %15s -> %15s (node %u, nodes %u/%u, reason %lu)", from, to, &node, &nodes,
                        &max_nodes, &reason) == 6) {
            std::snprintf(again, sizeof(again), "uds: role %s -> %s (node %u, nodes %u/%u, reason %lu)", from, to, node, nodes,
                          max_nodes, reason);
            CHECK(line == again);
            exact += line == again ? 1 : 0;
        }
    }
    CHECK_EQ(exact, 9);
    // The same lines are in the debug capture, for the log that has no main log next to it
    int in_capture = 0;
    int in_capture_exact = 0;
    DebugRecord rec;
    u8 buf[1700];
    while (DebugPop(&rec, buf, sizeof(buf))) {
        const std::string text(reinterpret_cast<char*>(buf), rec.stored);
        if (rec.kind == kDbgText && text.find("uds role ") == 0) {
            ++in_capture;
            char from[16], to[16];
            unsigned node = 0, nodes = 0, max_nodes = 0;
            unsigned long reason = 0;
            char again[160];
            if (std::sscanf(text.c_str(), "uds role %15s -> %15s node %u nodes %u/%u reason %lu", from, to, &node, &nodes,
                            &max_nodes, &reason) == 6) {
                std::snprintf(again, sizeof(again), "uds role %s -> %s node %u nodes %u/%u reason %lu", from, to, node, nodes,
                              max_nodes, reason);
                in_capture_exact += text == again ? 1 : 0;
            }
        }
    }
    CHECK_EQ(in_capture, 9);
    CHECK_EQ(in_capture_exact, 9);
    g_debug_level = 0;
}

// ---------------------------------------------------------------------------------------------
// Lifecycle (spec F01-F03, F12, F17, F18): the engine answers what the firmware answers, event by
// event and frame by frame. A Bench is a console (engine on the mock room) that counts the signals of
// its events and keeps the frames it sends.
// ---------------------------------------------------------------------------------------------
namespace {

constexpr u32 kW = 0xC8A113EA;
constexpr u32 kWO = 0xC9411002;
constexpr u32 kBadId = 0xE10113EA;
constexpr u32 kBig = 0xE10113E9;
constexpr u32 kArg = 0xE10113FD;
constexpr u32 kOom = 0xC86113F3;
constexpr u32 kNodeNotFound = 0xC90113FA;
constexpr u32 kConnectTimeout = 0xC92113FE;

// The 5 s ceiling of ConnectToNetwork would make every refused join a long test.
struct ShortWaitKernel : FakeKernel {
    bool WaitEvent(u32 h, s64 ns) override {
        return FakeKernel::WaitEvent(h, std::min<s64>(ns, cap_ns));
    }
    s64 cap_ns = 300000000; // (a test that needs a join to stay open raises it)
};

struct Bench {
    Bench(MockRoom& room, u8 mac_last) : console(room, mac_last, &kernel), sent(console) {}
    UdsEngine& e() {
        return *console.engine;
    }
    /// Initialize; the status event comes back in `status_event`.
    u32 Init(u16 version = kTestVersion, u32 shared_memory = kTestSharedMemory) {
        return e().Initialize(MakeNode(0x1000 + console.mac[5], "Bench", 0), shared_memory, version, &status_event);
    }
    ConnectionStatus Status() {
        return e().GetConnectionStatus();
    }
    u32 Signals(u32 event) {
        return kernel.Signals(event);
    }
    u32 BeginHosting(u8 max_nodes = 4) {
        NetworkInfo ni = MakeNetworkInfo(max_nodes, 6);
        return e().BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni));
    }
    /// Scans the beacons that arrived and returns the first network.
    bool ScanFirst(NetworkInfo* net) {
        std::vector<u8> scan(0x4000);
        return e().RecvBeaconBroadcastData(scan.data(), static_cast<u32>(scan.size()), kBroadcastMac) == 0 &&
               FirstScannedNetwork(scan, net);
    }

    ShortWaitKernel kernel;
    Console console;
    SentFrames sent;
    u32 status_event = 0;
};

/// Makes the engine of `host` send a beacon now (its timer is driven by the time it is given).
void SendBeacon(Bench& host) {
    static u64 later_ms = 0;
    later_ms += 100000;
    host.e().Tick(platform::NowMs() + later_ms);
}

/// `client` scans for the beacon of `host` and joins.
u32 JoinBench(MockRoom& room, Bench& client, Bench& host, u8 type = 1) {
    SendBeacon(host);
    room.WaitIdle();
    NetworkInfo net;
    if (!client.ScanFirst(&net)) {
        return 0xFFFFFFFF;
    }
    const u32 r = client.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), type);
    room.WaitIdle();
    return r;
}

} // namespace

static void TestInitializeSemantics() {
    std::printf("Initialize: shared memory, version, a second call\n");
    g_rules.Reset();
    MockRoom room;
    Bench a(room, 1);

    // the block the game gives is a multiple of 4 KiB
    CHECK_EQ(a.Init(kTestVersion, 0x14001), kOom);
    CHECK(!a.e().initialized());
    CHECK_EQ(a.status_event, 0u);
    // a version above 0x400 would crash nwm: the game gets the newest instead
    CHECK_EQ(a.Init(0x401), 0u);
    CHECK(a.e().initialized());
    CHECK(a.status_event != 0);
    const u32 first_event = a.status_event;

    // a second call (the game's second session) changes nothing: not the node, not a live network
    CHECK_EQ(a.BeginHosting(), 0u);
    CHECK_EQ(a.Signals(first_event), 1u); // BeginHosting
    u32 ev2 = 0;
    CHECK_EQ(a.e().Initialize(MakeNode(0x9999, "Other", 0), 0x2000, 0x100, &ev2), 0u);
    CHECK_EQ(ev2, first_event); // the same event again
    ConnectionStatus st = a.Status();
    CHECK_EQ(st.status, 6u);
    CHECK_EQ(st.network_node_id, 1);
    CHECK_EQ(st.total_nodes, 1);
    CHECK_EQ(st.node_bitmask, 1);
    CHECK_EQ(st.max_nodes, 4);
    CHECK_EQ(a.Signals(first_event), 1u);
    NodeInfo own;
    CHECK_EQ(a.e().GetNodeInformation(1, &own), 0u);
    CHECK_EQ(own.friend_code_seed, u64(0x1001)); // the first call's node, not the second's
    // ... not even a bad block size is looked at once initialised
    CHECK_EQ(a.e().Initialize(MakeNode(0x9999, "Other", 0), 0x2001, kTestVersion, &ev2), 0u);

    // Shutdown, Initialize, host again
    CHECK_EQ(a.e().Shutdown(), 0u);
    CHECK(!a.e().initialized());
    CHECK_EQ(a.Init(), 0u);
    ConnectionStatus fresh = a.Status();
    CHECK_EQ(fresh.status, 3u);
    CHECK_EQ(fresh.status_change_reason, 0u); // BEFORE_COMMUNICATION
    CHECK_EQ(fresh.node_bitmask, 0);
    CHECK_EQ(a.BeginHosting(), 0u);
    CHECK_EQ(a.Status().status_change_reason, 1u); // NETWORK_IS_AVAILABLE
    CHECK_RULES();
}

static void TestShutdownSemantics() {
    std::printf("Shutdown: idle, host with a client, client\n");
    g_rules.Reset();
    MockRoom room;
    Bench host(room, 1), alice(room, 2);
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(alice.Init(), 0u);

    // idle: nothing on the wire, no status event; the binds are freed (signalled, then closed)
    u32 bind_event = 0;
    CHECK_EQ(host.e().Bind(2, 0x2E30, 0xF3, 0xFFFF, &bind_event), 0u);
    CHECK_EQ(host.e().Shutdown(), 0u);
    CHECK_EQ(host.sent.Total(), std::size_t(0));
    CHECK_EQ(host.Signals(host.status_event), 0u);
    CHECK_EQ(host.Signals(bind_event), 1u);
    CHECK_EQ(host.e().Shutdown(), 0u); // a second one is a no-op
    CHECK_EQ(host.sent.Total(), std::size_t(0));
    CHECK_EQ(host.Status().status, 0u); // not initialised: no status of its own
    CHECK_EQ(host.e().DestroyNetwork(), kW);

    // host with a client: three Deauthentications to everybody, one status event, binds freed
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(host.BeginHosting(), 0u);
    CHECK_EQ(host.e().Bind(2, 0x2E30, 0xF3, 0xFFFF, &bind_event), 0u);
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    CHECK_EQ(host.Status().total_nodes, 2);
    host.sent.Clear();
    const u32 events = host.Signals(host.status_event);
    CHECK_EQ(host.e().Shutdown(), 0u);
    CHECK_EQ(host.sent.CountTo(WifiPacket::Type::Deauthentication, kBroadcastMac), 3);
    CHECK_EQ(host.sent.Total(), std::size_t(3));
    CHECK_EQ(host.Signals(host.status_event), events + 1);
    CHECK_EQ(host.Signals(bind_event), 2u); // a live session wakes its binds twice: at its end and when they are freed
    CHECK(!host.e().initialized());
    host.sent.Clear();
    CHECK_EQ(host.e().Shutdown(), 0u);
    CHECK_EQ(host.sent.Total(), std::size_t(0));
    CHECK_EQ(host.Signals(host.status_event), events + 1);
    room.WaitIdle();
    CHECK_EQ(alice.e().DisconnectNetwork(), 0u); // (the Azahar side has no use for the host's goodbye)

    // the client: one Deauthentication, to its host
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(host.BeginHosting(), 0u);
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    alice.sent.Clear();
    const u32 alice_events = alice.Signals(alice.status_event);
    CHECK_EQ(alice.e().Shutdown(), 0u);
    CHECK_EQ(alice.sent.Total(), std::size_t(1));
    CHECK_EQ(alice.sent.CountTo(WifiPacket::Type::Deauthentication, host.console.mac), 1);
    CHECK_EQ(alice.Signals(alice.status_event), alice_events + 1);
    room.WaitIdle();
    CHECK_EQ(host.Status().total_nodes, 1); // the host heard it

    // nothing is sent for a game that left the Deauthentications off, or initialised an old way
    CHECK_EQ(host.e().SetDeauthOnDestroy(false), 0u);
    host.sent.Clear();
    CHECK_EQ(host.e().Shutdown(), 0u);
    CHECK_EQ(host.sent.Total(), std::size_t(0));
    CHECK_EQ(host.Init(0x1FF), 0u); // (below 0x200: the burst does not exist yet)
    CHECK_EQ(host.BeginHosting(), 0u);
    CHECK_EQ(host.e().DestroyNetwork(), 0u);
    CHECK_EQ(host.sent.Total(), std::size_t(0));
    CHECK_EQ(host.Status().status_change_reason, 2u); // but it ended all the same
    CHECK_RULES();
}

static void TestDestroyDisconnect() {
    std::printf("DestroyNetwork / DisconnectNetwork: reasons, events, Deauthentications\n");
    g_rules.Reset();
    MockRoom room;
    Bench host(room, 1), alice(room, 2);
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(alice.Init(), 0u);

    // Disconnect with no session (libctru calls it after every failed connect): nothing happens
    CHECK_EQ(alice.e().DisconnectNetwork(), 0u);
    CHECK_EQ(alice.sent.Total(), std::size_t(0));
    CHECK_EQ(alice.Signals(alice.status_event), 0u);
    CHECK_EQ(alice.Status().status_change_reason, 0u);

    // a join nobody answers: status 3 / reason 0 / the status event once, the answer is the timeout;
    // the Disconnect libctru sends next is again nothing
    NetworkInfo nobody = MakeNetworkInfo(4, 0);
    nobody.host_mac_address = Mac{0, 0x1F, 0x32, 0, 0, 0x55};
    nobody.channel = 11;
    CHECK_EQ(alice.e().ConnectToNetwork(reinterpret_cast<u8*>(&nobody), sizeof(nobody), 1), kConnectTimeout);
    CHECK_EQ(alice.Status().status, 3u);
    CHECK_EQ(alice.Status().status_change_reason, 0u);
    CHECK_EQ(alice.Signals(alice.status_event), 1u);
    alice.sent.Clear();
    CHECK_EQ(alice.e().DisconnectNetwork(), 0u);
    CHECK_EQ(alice.sent.Total(), std::size_t(0));
    CHECK_EQ(alice.Signals(alice.status_event), 1u);

    // a host that is up cannot be disconnected from, and keeps its NetworkInfo when somebody calls Connect
    CHECK_EQ(host.BeginHosting(), 0u);
    u8 channel = 0;
    CHECK_EQ(host.e().GetChannel(&channel), 0u);
    CHECK_EQ(channel, 11);
    CHECK_EQ(host.e().ConnectToNetwork(reinterpret_cast<u8*>(&nobody), sizeof(nobody), 1), kW);
    CHECK_EQ(host.e().BeginHostingNetwork(reinterpret_cast<u8*>(&nobody), sizeof(nobody)), kW);
    CHECK_EQ(host.Status().status, 6u);
    CHECK_EQ(host.e().GetChannel(&channel), 0u);
    CHECK_EQ(channel, 11);
    u8 app[8];
    u32 app_size = 0;
    CHECK_EQ(host.e().GetApplicationData(app, sizeof(app), &app_size), 0u);
    CHECK_EQ(app_size, 6u); // the host's own application data, not the one of the refused Connect

    // a client: Disconnect sends one Deauthentication to the host, ends with reason 2, signals once
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    u32 bind_event = 0;
    CHECK_EQ(alice.e().Bind(2, 0x2E30, 0xF3, 0xFFFF, &bind_event), 0u);
    alice.sent.Clear();
    const u32 events = alice.Signals(alice.status_event);
    CHECK_EQ(alice.e().DisconnectNetwork(), 0u);
    CHECK_EQ(alice.sent.Total(), std::size_t(1));
    CHECK_EQ(alice.sent.CountTo(WifiPacket::Type::Deauthentication, host.console.mac), 1);
    CHECK_EQ(alice.Signals(alice.status_event), events + 1);
    CHECK_EQ(alice.Signals(bind_event), 1u); // whoever waits for data wakes up
    ConnectionStatus st = alice.Status();
    CHECK_EQ(st.status, 3u);
    CHECK_EQ(st.status_change_reason, 2u);
    CHECK_EQ(st.network_node_id, 2); // the id stays readable
    CHECK_EQ(st.total_nodes, 0);
    CHECK_EQ(st.node_bitmask, 0);
    u16 src = 0;
    u32 size = 0;
    std::vector<u8> out;
    CHECK_EQ(PullInto(alice.e(), 2, &out, &size, &src), kW);
    room.WaitIdle();
    CHECK_EQ(host.Status().total_nodes, 1);

    // the host: Disconnect wipes the tables and answers "wrong state"; Destroy ends the network with
    // three Deauthentications, reason 2, one status event
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    CHECK_EQ(host.Status().total_nodes, 2);
    CHECK_EQ(host.e().DisconnectNetwork(), kW);
    st = host.Status();
    CHECK_EQ(st.status, 6u);
    CHECK_EQ(st.total_nodes, 0);
    CHECK_EQ(st.node_bitmask, 0);
    CHECK_EQ(st.status_change_reason, 1u);
    CHECK_EQ(host.e().DestroyNetwork(), 0u);
    host.sent.Clear();
    CHECK_EQ(host.BeginHosting(), 0u);
    const u32 host_events = host.Signals(host.status_event);
    host.sent.Clear();
    CHECK_EQ(host.e().DestroyNetwork(), 0u);
    CHECK_EQ(host.sent.CountTo(WifiPacket::Type::Deauthentication, kBroadcastMac), 3);
    CHECK_EQ(host.sent.Total(), std::size_t(3));
    CHECK_EQ(host.Signals(host.status_event), host_events + 1);
    st = host.Status();
    CHECK_EQ(st.status, 3u);
    CHECK_EQ(st.status_change_reason, 2u);
    CHECK_EQ(st.network_node_id, 1);
    CHECK_EQ(host.e().DestroyNetwork(), kW);
    CHECK_RULES();
}

static void TestConnectArguments() {
    std::printf("ConnectToNetwork / BeginHostingNetwork: state and argument checks\n");
    g_rules.Reset();
    MockRoom room;
    Bench host(room, 1), alice(room, 2);
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(alice.Init(), 0u);
    NetworkInfo net = MakeNetworkInfo(4, 0);
    net.host_mac_address = Mac{0, 0x1F, 0x32, 0, 0, 0x55};
    net.channel = 11;
    // a connection type the service does not have: argument error, nothing changed, nothing sent
    CHECK_EQ(alice.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 0), kArg);
    CHECK_EQ(alice.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 3), kArg);
    CHECK_EQ(alice.Status().status, 3u);
    CHECK_EQ(alice.sent.Total(), std::size_t(0));
    // a host with a bad max_nodes does not lose the NetworkInfo it had (idle: channel 0)
    NetworkInfo bad = MakeNetworkInfo(1, 4);
    CHECK_EQ(host.e().BeginHostingNetwork(reinterpret_cast<u8*>(&bad), sizeof(bad)), kBadId);
    NetworkInfo big = MakeNetworkInfo(4, 4);
    big.application_data_size = 201;
    CHECK_EQ(host.e().BeginHostingNetwork(reinterpret_cast<u8*>(&big), sizeof(big)), kBig);
    u8 channel = 0xEE;
    CHECK_EQ(host.e().GetChannel(&channel), 0u);
    CHECK_EQ(channel, 0); // nothing was hosted or joined yet
    CHECK_EQ(host.Status().status, 3u);
    // a second BeginHosting, and one as a client, are "wrong state" and leave the session alone
    CHECK_EQ(host.BeginHosting(), 0u);
    CHECK_EQ(host.BeginHosting(), kW);
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    NetworkInfo other = MakeNetworkInfo(2, 0);
    CHECK_EQ(alice.e().BeginHostingNetwork(reinterpret_cast<u8*>(&other), sizeof(other)), kW);
    CHECK_EQ(alice.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1), kW);
    CHECK_EQ(alice.Status().status, 9u);
    CHECK_EQ(alice.Status().max_nodes, 4);
    CHECK_EQ(host.Status().total_nodes, 2);
    CHECK_RULES();
}

static void TestEjectClient() {
    std::printf("EjectClient: node 1, non-host, one node, everybody\n");
    g_rules.Reset();
    MockRoom room;
    Bench host(room, 1), alice(room, 2), bob(room, 3);
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(alice.Init(), 0u);
    CHECK_EQ(bob.Init(), 0u);
    // before there is a network: "wrong state" (the node 1 test comes first, and only it is a usage error)
    CHECK_EQ(host.e().EjectClient(2), kW);
    CHECK_EQ(host.e().EjectClient(1), kBadId);
    CHECK_EQ(host.BeginHosting(), 0u);
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    CHECK_EQ(JoinBench(room, bob, host), 0u);
    CHECK_EQ(host.Status().total_nodes, 3);
    CHECK_EQ(alice.e().EjectClient(3), kW); // clients can't
    CHECK_EQ(alice.e().EjectClient(1), kBadId);
    CHECK_EQ(host.e().EjectClient(1), kBadId);

    host.sent.Clear();
    CHECK_EQ(host.e().EjectClient(9), 0u); // unknown node: nothing
    CHECK_EQ(host.e().EjectClient(0), 0u);
    CHECK_EQ(host.sent.Total(), std::size_t(0));

    const u32 events = host.Signals(host.status_event);
    CHECK_EQ(host.e().EjectClient(2), 0u);
    CHECK_EQ(host.sent.Total(), std::size_t(1));
    CHECK_EQ(host.sent.CountTo(WifiPacket::Type::Deauthentication, alice.console.mac), 1);
    ConnectionStatus st = host.Status();
    CHECK_EQ(st.total_nodes, 2);
    CHECK_EQ(st.node_bitmask, 5); // nodes 1 and 3
    CHECK_EQ(st.nodes[1], 0);
    CHECK_EQ(st.changed_nodes, 2); // node 2 changed (the joins were read above)
    CHECK_EQ(host.Signals(host.status_event), events + 1);
    room.WaitIdle();
    CHECK_EQ(host.e().EjectClient(2), 0u); // gone: unknown now

    host.sent.Clear();
    CHECK_EQ(host.e().EjectClient(0xFFFF), 0u);
    CHECK_EQ(host.sent.CountTo(WifiPacket::Type::Deauthentication, kBroadcastMac), 3);
    CHECK_EQ(host.sent.Total(), std::size_t(3));
    st = host.Status();
    CHECK_EQ(st.total_nodes, 1);
    CHECK_EQ(st.node_bitmask, 1);
    CHECK_EQ(st.changed_nodes, 4); // node 3 left
    CHECK_EQ(host.Signals(host.status_event), events + 2);
    CHECK_EQ(host.Status().status, 6u); // the host is still up
    CHECK_RULES();
}

static void TestSendToSemantics() {
    std::printf("SendTo: error order, destinations that are not there, unicast routing\n");
    g_rules.Reset();
    MockRoom room;
    NetworkInfo ni = MakeNetworkInfo(4, 4);
    AzaharHost azhost(room, 1, ni, MakeNode(0x1111, "AzHost", 0));
    Bench alice(room, 2), bob(room, 3);
    CHECK_EQ(alice.Init(), 0u);
    CHECK_EQ(bob.Init(), 0u);
    const u8 msg[] = "unicast";
    // not in a network: wrong state, whatever the destination (the size is checked before the state)
    CHECK_EQ(alice.e().SendTo(1, 1, sizeof(msg), 0, msg, sizeof(msg)), kW);
    std::vector<u8> big(0x5C7, 0);
    CHECK_EQ(alice.e().SendTo(1, 1, 0x5C7, 0, big.data(), big.size()), kBig);

    azhost.SendBeacon();
    room.WaitIdle();
    NetworkInfo net;
    CHECK(alice.ScanFirst(&net));
    CHECK_EQ(alice.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1), 0u);
    room.WaitIdle(); // (the Logoff that admitted alice is still on its way to bob, who must not take it for his)
    CHECK_EQ(bob.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1), 0u);
    room.WaitIdle();
    CHECK_EQ(alice.Status().network_node_id, 2);
    u32 bob_bind = 0;
    CHECK_EQ(bob.e().Bind(2, 0x2E30, 1, 0xFFFF, &bob_bind), 0u);
    azhost.TakeInbox();

    // the host's MAC for node 1; the destination node's own MAC for a node alice knows
    CHECK_EQ(alice.e().SendTo(1, 1, sizeof(msg), 0, msg, sizeof(msg)), 0u);
    room.WaitIdle();
    CHECK_EQ(azhost.TakeInbox().size(), std::size_t(1));
    CHECK_EQ(alice.e().SendTo(3, 1, sizeof(msg), 0, msg, sizeof(msg)), 0u);
    room.WaitIdle();
    CHECK_EQ(azhost.forwarded(), 0u); // went to bob's MAC: the host never saw it
    u16 src = 0;
    u32 size = 0;
    PullAll(bob.console, 2, &src, &size);
    CHECK_EQ(size, u32(sizeof(msg)));
    CHECK_EQ(src, 2);

    // a node that is not there, or is yourself: no frame for the first, the firmware's error for the second
    alice.sent.Clear();
    CHECK_EQ(alice.e().SendTo(0, 1, sizeof(msg), 0, msg, sizeof(msg)), 0u);
    CHECK_EQ(alice.e().SendTo(4, 1, sizeof(msg), 0, msg, sizeof(msg)), 0u);
    CHECK_EQ(alice.e().SendTo(12, 1, sizeof(msg), 0, msg, sizeof(msg)), 0u);
    CHECK_EQ(alice.sent.Total(), std::size_t(0));
    CHECK_EQ(alice.e().SendTo(2, 1, sizeof(msg), 0, msg, sizeof(msg)), kNodeNotFound);
    CHECK_EQ(alice.sent.Total(), std::size_t(0));
    // the broadcast flag and the broadcast node go to the broadcast MAC, existing node or not
    CHECK_EQ(alice.e().SendTo(9, 1, sizeof(msg), 2, msg, sizeof(msg)), 0u);
    CHECK_EQ(alice.e().SendTo(0xFFFF, 1, sizeof(msg), 0, msg, sizeof(msg)), 0u);
    CHECK_EQ(alice.sent.CountSecureData(), 2);
    room.WaitIdle();
    azhost.TakeInbox();

    // a node the game's status block lists but whose MAC alice never learnt (the node list and the
    // Logoff race each other): the frame goes to the host, which relays it, exactly once
    std::vector<NodeInfo> nodes(4);
    nodes[0] = MakeNode(0x1111, "AzHost", 1);
    nodes[1] = MakeNode(0x1002, "Bench", 2);
    nodes[3] = MakeNode(0x4444, "Late", 4);
    WifiPacket logoff;
    logoff.type = WifiPacket::Type::Data;
    logoff.channel = 11;
    logoff.transmitter_address = azhost.mac();
    logoff.destination_address = kBroadcastMac;
    logoff.data = GenerateEapolLogoffFrame(2, nodes, 4, 3);
    alice.e().OnWifiPacket(logoff);
    CHECK((alice.Status().node_bitmask & 8) != 0);
    alice.sent.Clear();
    const u32 forwarded = azhost.forwarded();
    CHECK_EQ(alice.e().SendTo(4, 1, sizeof(msg), 0, msg, sizeof(msg)), 0u);
    CHECK_EQ(alice.sent.CountTo(WifiPacket::Type::Data, azhost.mac()), 1);
    room.WaitIdle();
    CHECK_EQ(azhost.forwarded(), forwarded + 1);
    CHECK_RULES();
}

static void TestNetworkAttributes() {
    std::printf("UpdateNetworkAttribute / EjectSpectators: the lobby closes and the beacon says so\n");
    g_rules.Reset();
    MockRoom room;
    Bench host(room, 1), alice(room, 2), bob(room, 3);
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(alice.Init(), 0u);
    CHECK_EQ(bob.Init(), 0u);
    CHECK_EQ(host.e().UpdateNetworkAttribute(2, 1), 0u); // (no network yet: no state needed)
    CHECK_EQ(host.e().EjectSpectators(), kW);
    CHECK_EQ(host.BeginHosting(), 0u); // advertises 0x0002, but nothing is enforced before an update
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    NetworkInfo net;
    SendBeacon(host);
    room.WaitIdle();
    CHECK(bob.ScanFirst(&net));
    CHECK_EQ(LoadBE16(net.attributes_be), 0x0002);

    // libctru's udsSetNewConnectionsBlocked(true, true, false): bit 1 blocks new clients; the one that
    // is in stays, the beacon shows the bit, a new join is not answered
    CHECK_EQ(host.e().UpdateNetworkAttribute(2, 1), 0u);
    SendBeacon(host);
    room.WaitIdle();
    CHECK(bob.ScanFirst(&net));
    CHECK_EQ(LoadBE16(net.attributes_be), 0x0002);
    host.sent.Clear();
    CHECK_EQ(bob.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1), kConnectTimeout);
    room.WaitIdle();
    CHECK_EQ(host.sent.Count(WifiPacket::Type::Authentication), 0);
    CHECK_EQ(host.Status().total_nodes, 2);
    CHECK_EQ(alice.Status().status, 9u);
    // ... and opens again
    CHECK_EQ(host.e().UpdateNetworkAttribute(2, 0), 0u);
    SendBeacon(host);
    room.WaitIdle();
    CHECK(bob.ScanFirst(&net));
    CHECK_EQ(LoadBE16(net.attributes_be), 0x0000);
    CHECK_EQ(bob.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1), 0u);
    room.WaitIdle();
    CHECK_EQ(host.Status().total_nodes, 3);

    // bit 2 is the "closed for everybody" bit and survives only a call that names it
    CHECK_EQ(host.e().UpdateNetworkAttribute(4, 1), 0u);
    CHECK_EQ(bob.e().DisconnectNetwork(), 0u);
    room.WaitIdle();
    SendBeacon(host);
    room.WaitIdle();
    CHECK(bob.ScanFirst(&net));
    CHECK_EQ(LoadBE16(net.attributes_be), 0x0004);
    CHECK_EQ(bob.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1), kConnectTimeout);
    CHECK_EQ(host.e().UpdateNetworkAttribute(1, 1), 0u); // a call that doesn't name bit 2 clears it
    SendBeacon(host);
    room.WaitIdle();
    CHECK(bob.ScanFirst(&net));
    CHECK_EQ(LoadBE16(net.attributes_be), 0x0001);
    CHECK_EQ(bob.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1), 0u);

    // EjectSpectators closes the lobby to spectators (bit 0); only a host can
    CHECK_EQ(alice.e().EjectSpectators(), kW);
    CHECK_EQ(host.e().UpdateNetworkAttribute(1, 0), 0u);
    CHECK_EQ(host.e().EjectSpectators(), 0u);
    Bench carol(room, 4);
    CHECK_EQ(carol.Init(), 0u);
    SendBeacon(host);
    room.WaitIdle();
    CHECK(carol.ScanFirst(&net));
    CHECK_EQ(LoadBE16(net.attributes_be), 0x0001);
    CHECK_RULES();
}

static void TestServiceStop() {
    std::printf("StopService: sleep and the wireless switch end the session and refuse everything until Shutdown\n");
    g_rules.Reset();
    MockRoom room;
    Bench host(room, 1), alice(room, 2);
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(alice.Init(), 0u);
    CHECK(!host.e().StopService(kServiceSleep)); // idle: only the status event, once
    CHECK_EQ(host.Signals(host.status_event), 1u);
    CHECK_EQ(host.sent.Total(), std::size_t(0));
    CHECK_EQ(host.e().Shutdown(), 0u);

    for (const int kind : {kServiceSleep, kServiceWirelessOff, kServiceShutdown}) {
        CHECK_EQ(host.Init(), 0u);
        CHECK_EQ(host.BeginHosting(), 0u);
        u32 bind_event = 0;
        CHECK_EQ(host.e().Bind(2, 0x2E30, 0xF3, 0xFFFF, &bind_event), 0u);
        if (!alice.e().initialized()) {
            CHECK_EQ(alice.Init(), 0u);
        }
        CHECK_EQ(JoinBench(room, alice, host), 0u);
        host.sent.Clear();
        const u32 events = host.Signals(host.status_event);
        std::vector<std::string> said;
        SetLogSink([&said](const char* line) { said.push_back(line); });
        g_debug_level = 1;
        CHECK(host.e().StopService(kind)); // a session was up
        g_debug_level = 0;
        SetLogSink([](const char*) {});
        // (the line is made by hand, azl/line.hpp: the text is the printf format's of 0.1.15)
        const char* const what = kind == kServiceWirelessOff ? "wireless off" : kind == kServiceSleep ? "sleep" : "shutdown";
        const std::string expected = std::string("uds: the service stops (") + what + ") while host";
        CHECK(std::find(said.begin(), said.end(), expected) != said.end());
        CHECK_EQ(host.sent.CountTo(WifiPacket::Type::Deauthentication, kBroadcastMac), 3);
        CHECK_EQ(host.Signals(host.status_event), events + 2); // (the end of the session, and the task exiting)
        CHECK_EQ(host.Signals(bind_event), 2u); // (at the end of the session, and again when it is freed)
        CHECK(host.e().initialized()); // still initialised: only Shutdown gets out
        const u32 refused = kind == kServiceWirelessOff ? kWO : kW;
        ConnectionStatus st = host.Status();
        CHECK_EQ(st.status, 11u);
        CHECK_EQ(st.status_change_reason, 3u); // REQUEST_FROM_SYSTEM
        CHECK_EQ(st.total_nodes, 0);
        CHECK_EQ(host.BeginHosting(), refused);
        CHECK_EQ(host.Init(), refused);
        CHECK_EQ(host.e().DestroyNetwork(), refused);
        CHECK_EQ(host.e().Bind(3, 0x2E30, 0xF4, 0xFFFF, &bind_event), refused);
        u16 src = 0;
        u32 size = 0;
        std::vector<u8> out;
        CHECK_EQ(PullInto(host.e(), 2, &out, &size, &src), refused);
        CHECK_EQ(host.e().Unbind(2), 0u);
        u8 channel;
        CHECK_EQ(host.e().GetChannel(&channel), refused);
        room.WaitIdle();
        CHECK_EQ(host.e().Shutdown(), 0u); // the way out
        CHECK_EQ(host.Init(), 0u);
        CHECK_EQ(host.Status().status, 3u);
        CHECK_EQ(host.BeginHosting(), 0u);
        CHECK_EQ(host.e().Shutdown(), 0u);
        room.WaitIdle();
        if (alice.e().initialized()) {
            CHECK_EQ(alice.e().Shutdown(), 0u);
        }
    }

    // a client that is stopped says goodbye to its host and a Connect in progress fails
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(host.BeginHosting(), 0u);
    CHECK_EQ(alice.Init(), 0u);
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    alice.sent.Clear();
    CHECK(alice.e().StopService(kServiceSleep));
    CHECK_EQ(alice.sent.CountTo(WifiPacket::Type::Deauthentication, host.console.mac), 1);
    room.WaitIdle();
    CHECK_EQ(host.Status().total_nodes, 1);
    // a service that was never initialised is not stopped by anything
    Bench idle(room, 5);
    CHECK(!idle.e().StopService(kServiceSleep));
    CHECK_EQ(idle.Init(), 0u);
    CHECK_RULES();
}

// A client whose host has answered its Authentication tells the host when it gives up (the host keeps
// a half-open entry for it otherwise); one whose host never answered has nobody to tell.
static void TestConnectGivenUp() {
    std::printf("a Connect that is given up: Deauthentication only if the host answered\n");
    g_rules.Reset();
    MockRoom room;
    Bench alice(room, 2);
    CHECK_EQ(alice.Init(), 0u);
    const Mac host{0x00, 0x1F, 0x32, 0x00, 0x00, 0x66};
    g_rules.AddMember(host);
    NetworkInfo net = MakeNetworkInfo(4, 0);
    net.host_mac_address = host;
    net.channel = 11;
    auto from_host = [&](WifiPacket::Type type, std::vector<u8> data) {
        WifiPacket p;
        p.type = type;
        p.channel = 11;
        p.transmitter_address = host;
        p.destination_address = alice.console.mac;
        p.data = std::move(data);
        alice.e().OnWifiPacket(p);
    };
    auto wait_for = [&](WifiPacket::Type type, int count) {
        for (int i = 0; i < 1000 && alice.sent.Count(type) < count; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return alice.sent.Count(type) >= count;
    };

    for (const bool answered : {true, false}) {
        for (const bool by_disconnect : {true, false}) {
            alice.sent.Clear();
            u32 result = 0xFFFFFFFF;
            std::thread connecting([&] {
                result = alice.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1);
            });
            CHECK(wait_for(WifiPacket::Type::Authentication, 1));
            if (answered) { // the Authentication reply and the association: the EAPoL-Start follows
                from_host(WifiPacket::Type::Authentication, GenerateAuthenticationFrame(AuthSeq::Seq2));
                from_host(WifiPacket::Type::AssociationResponse, GenerateAssocResponseFrame(0, 1, 0xDEADBEEF));
                CHECK(wait_for(WifiPacket::Type::Data, 1));
            }
            if (by_disconnect) { // the game cancels
                CHECK_EQ(alice.e().DisconnectNetwork(), 0u);
            }
            connecting.join(); // (or the 0.3 s of the test kernel pass: nobody sends the Logoff)
            CHECK_EQ(result, by_disconnect ? 0xC92113FBu : kConnectTimeout);
            CHECK_EQ(alice.Status().status, 3u);
            CHECK_EQ(alice.Status().status_change_reason, by_disconnect ? 2u : 0u);
            CHECK_EQ(alice.sent.CountTo(WifiPacket::Type::Deauthentication, host), answered ? 1 : 0);
        }
    }
    // Shutdown while connecting: the same
    alice.sent.Clear();
    std::thread connecting([&] { alice.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1); });
    CHECK(wait_for(WifiPacket::Type::Authentication, 1));
    from_host(WifiPacket::Type::Authentication, GenerateAuthenticationFrame(AuthSeq::Seq2));
    from_host(WifiPacket::Type::AssociationResponse, GenerateAssocResponseFrame(0, 1, 0xDEADBEEF));
    CHECK_EQ(alice.e().Shutdown(), 0u);
    connecting.join();
    CHECK_EQ(alice.sent.CountTo(WifiPacket::Type::Deauthentication, host), 1);
    CHECK_RULES();
}

// A Connect that a Disconnect woke up may run after the game began another Connect on another thread (one
// core, the cancelling thread keeps the CPU): the new attempt is not the old thread's to time out or tear down.
static void TestConnectWokenLate() {
    std::printf("a Connect woken by a Disconnect that runs after a new Connect began leaves the new one alone\n");
    g_rules.Reset();
    MockRoom room;
    // The first Connect to be woken (the kernel's wait returned true) is held there until the test lets it go.
    struct ParkKernel : ShortWaitKernel {
        bool WaitEvent(u32 h, s64 ns) override {
            const bool woken = ShortWaitKernel::WaitEvent(h, ns);
            if (woken && arm.exchange(false)) {
                parked = true;
                while (!release.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            return woken;
        }
        std::atomic<bool> arm{false}, parked{false}, release{false};
    } kernel;
    kernel.cap_ns = 2000000000; // the second attempt stays open for 2 s: long enough on a loaded machine
    Console alice(room, 2, &kernel);
    u32 event = 0;
    CHECK_EQ(alice.engine->Initialize(MakeNode(0x2002, "Alice", 0), kTestSharedMemory, kTestVersion, &event), 0u);
    const Mac host{0x00, 0x1F, 0x32, 0x00, 0x00, 0x66}; // nobody answers
    g_rules.AddMember(host);
    NetworkInfo net = MakeNetworkInfo(4, 0);
    net.host_mac_address = host;
    net.channel = 11;
    auto spin = [](const std::function<bool()>& done) {
        for (int i = 0; i < 5000 && !done(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return done();
    };
    auto connect = [&](u32* result) {
        return std::thread([&alice, &net, result] {
            *result = alice.engine->ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1);
        });
    };

    u32 first = 0xFFFFFFFF, second = 0xFFFFFFFF;
    kernel.arm = true;
    std::thread t1 = connect(&first);
    CHECK(spin([&] { return alice.engine->status() == NetworkStatus::Connecting; }));
    CHECK_EQ(alice.engine->DisconnectNetwork(), 0u); // wakes the first thread, which is held before it can run
    CHECK(spin([&] { return kernel.parked.load(); }));
    CHECK(alice.engine->status() == NetworkStatus::NotConnected);
    std::thread t2 = connect(&second); // the game tries again
    CHECK(spin([&] { return alice.engine->status() == NetworkStatus::Connecting; }));
    kernel.release = true;
    t1.join();
    CHECK_EQ(first, 0xC92113FBu);                                  // cut short, as the Disconnect left it
    CHECK(alice.engine->status() == NetworkStatus::Connecting);    // the second attempt was not torn down ...
    CHECK_EQ(alice.engine->DisconnectNetwork(), 0u);               // ... and ends by its own Disconnect
    t2.join();
    CHECK_EQ(second, 0xC92113FBu);
    CHECK(alice.engine->status() == NetworkStatus::NotConnected);
    CHECK_RULES();
}

// A client counts the beacons of its host and the longest silence (F26 instrumentation).
static void TestHostBeaconAge() {
    std::printf("host beacons of a client: count and longest silence\n");
    g_rules.Reset();
    MockRoom room;
    NetworkInfo ni = MakeNetworkInfo(4, 4);
    AzaharHost azhost(room, 1, ni, MakeNode(0x1111, "AzHost", 0));
    Bench alice(room, 2);
    CHECK_EQ(alice.Init(), 0u);
    azhost.SendBeacon();
    room.WaitIdle();
    NetworkInfo net;
    CHECK(alice.ScanFirst(&net));
    std::memset(&g_stats, 0, sizeof(g_stats));
    azhost.SendBeacon(); // idle: not counted, nobody's host yet
    room.WaitIdle();
    CHECK_EQ(g_stats.host_beacons, 0u);
    CHECK_EQ(alice.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1), 0u);
    room.WaitIdle();
    azhost.SendBeacon();
    azhost.SendBeacon();
    room.WaitIdle();
    CHECK_EQ(g_stats.host_beacons, 2u);
    alice.e().Tick(platform::NowMs() + 1500); // 1.5 s without a beacon
    CHECK(g_stats.host_beacon_age_max >= 1400 && g_stats.host_beacon_age_max <= 1700);
    // a frame from somebody else is not the host's beacon
    Console stranger(room, 7);
    WifiPacket beacon;
    beacon.type = WifiPacket::Type::Beacon;
    beacon.channel = 11;
    beacon.destination_address = kBroadcastMac;
    beacon.transmitter_address = stranger.mac;
    beacon.data = GenerateBeaconFrame(azhost.network(), std::vector<NodeInfo>(4));
    alice.e().OnWifiPacket(beacon);
    CHECK_EQ(g_stats.host_beacons, 2u);
    // the clock stops when the session ends
    CHECK_EQ(alice.e().DisconnectNetwork(), 0u);
    std::memset(&g_stats, 0, sizeof(g_stats));
    alice.e().Tick(platform::NowMs() + 9000);
    CHECK_EQ(g_stats.host_beacon_age_max, 0u);
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------
// Scan fidelity (spec F04, C7): the scan listens first, reports what it heard, consumes nothing,
// stops for the game's cancel event, and counts (not enforces) the wlancommid / id8 words.
// ---------------------------------------------------------------------------------------------
namespace {

/// A kernel whose scan sleeps can be seen: it counts them (and really sleeps if asked to), runs a hook
/// after each, and counts the events that were closed.
struct ScanKernel : FakeKernel {
    void SleepNs(s64 ns) override {
        const int n = ++sleeps;
        slept_ns += ns;
        if (real_time) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
        }
        if (on_sleep) {
            on_sleep(n);
        }
    }
    void CloseEvent(u32 h) override {
        ++closes;
        FakeKernel::CloseEvent(h);
    }
    bool real_time = false;
    std::atomic<int> sleeps{0};       ///< calls (the hooks count them)
    std::atomic<s64> slept_ns{0};     ///< what they asked for, added up: the length of the scan, whatever the slicing
    std::atomic<int> closes{0};
    std::function<void(int)> on_sleep;
};

/// The beacons of an Azahar host, every 100 ms, like its timer.
struct HostBeaconer {
    explicit HostBeaconer(AzaharHost& h) : host(h), thread([this] {
        while (!stop.load()) {
            host.SendBeacon();
            for (int i = 0; i < 10 && !stop.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }) {}
    ~HostBeaconer() {
        Stop();
    }
    void Stop() {
        stop = true;
        if (thread.joinable()) {
            thread.join();
        }
    }
    AzaharHost& host;
    std::atomic<bool> stop{false};
    std::thread thread;
};

struct ScanOutcome {
    u32 rc;
    u32 entries;
    std::vector<u8> buffer;
};
ScanOutcome DoScan(UdsEngine& engine, const ScanParams& params) {
    ScanOutcome out;
    out.buffer.assign(0x4000, 0);
    out.rc = engine.RecvBeaconBroadcastData(out.buffer.data(), static_cast<u32>(out.buffer.size()), params);
    out.entries = out.rc == 0 ? LoadLE32(out.buffer.data() + 8) : 0;
    return out;
}

/// A scan that only reports what was heard in the last beacon interval.
ScanParams NoWait() {
    ScanParams p;
    p.channel_mask = 0;
    return p;
}

double MsSince(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

static void TestScanFidelity() {
    std::printf("scan: listens first, hears what arrives, keeps the list, stops for the cancel event\n");
    g_rules.Reset();
    MockRoom room;
    NetworkInfo ni = MakeNetworkInfo(4, 6); // wlancommid 0x0010CAFE, id8 1
    AzaharHost azhost(room, 1, ni, MakeNode(0x1111, "AzHost", 0));
    u32 ev;
    auto refresh = [&] { // the host's beacon, heard now
        azhost.SendBeacon();
        room.WaitIdle();
    };

    // libctru's scan (3 channels x 110 ms) takes 330 ms, and a beacon that arrives in the middle of it
    // is reported (the old scan took its list first and slept afterwards)
    {
        ScanKernel kernel;
        kernel.real_time = true;
        Console alice(room, 2, &kernel);
        CHECK_EQ(alice.engine->Initialize(MakeNode(0x2222, "Alice", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        room.WaitIdle();
        kernel.on_sleep = [&](int n) {
            if (n == 5) {
                azhost.SendBeacon();
            }
        };
        const auto t0 = std::chrono::steady_clock::now();
        const ScanOutcome r = DoScan(*alice.engine, ScanParams());
        const double ms = MsSince(t0);
        CHECK_EQ(r.rc, 0u);
        CHECK_EQ(r.entries, 1u);
        CHECK(ms >= 325 && ms <= 600);
        CHECK(kernel.slept_ns.load() > 0 && kernel.slept_ns.load() <= 330000000); // (never more than the dwell asked)
        CHECK_EQ(LoadLE32(r.buffer.data() + 0), 0x4000u);

        // two scans in a row both find a host that keeps beaconing (the list is not consumed)
        kernel.on_sleep = nullptr;
        HostBeaconer beacons(azhost);
        CHECK_EQ(DoScan(*alice.engine, ScanParams()).entries, 1u);
        CHECK_EQ(DoScan(*alice.engine, ScanParams()).entries, 1u);
        // ... and one that stopped beaconing is gone a beacon interval later, and forgotten after 3 s
        beacons.Stop();
        refresh(); // (the last beacon of the thread may be 100 ms old already: one more, then silence)
        CHECK_EQ(DoScan(*alice.engine, NoWait()).entries, 1u); // (heard a moment ago)
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK_EQ(DoScan(*alice.engine, NoWait()).entries, 0u);
        std::this_thread::sleep_for(std::chrono::milliseconds(3100));
        CHECK_EQ(DoScan(*alice.engine, NoWait()).entries, 0u);
        CHECK_EQ(DoScan(*alice.engine, ScanParams()).entries, 0u); // a full scan hears nothing either
    }
    room.WaitIdle();

    // the first scan after Initialize finds a host that was beaconing before it
    {
        ScanKernel kernel;
        Console late(room, 3, &kernel);
        refresh();
        CHECK_EQ(late.engine->Initialize(MakeNode(0x3333, "Late", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        CHECK_EQ(DoScan(*late.engine, NoWait()).entries, 1u);
    }

    // the cancel event: 0, a handle that does not exist and one nobody signals let the scan run its
    // whole time; a signalled one ends it at once, and the game's event is never closed
    {
        ScanKernel kernel;
        Console c(room, 4, &kernel);
        CHECK_EQ(c.engine->Initialize(MakeNode(0x4444, "Cancel", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        ScanParams p;
        const u32 quiet_event = kernel.CreateEvent();
        for (const u32 cancel : {0u, 0xDEADu, quiet_event}) {
            kernel.slept_ns = 0;
            p.cancel = cancel;
            CHECK_EQ(DoScan(*c.engine, p).rc, 0u);
            CHECK_EQ(kernel.slept_ns.load(), 330000000); // 3 channels x 110 ms, all of it
        }
        const u32 game_event = kernel.CreateEvent();
        p.cancel = game_event;
        kernel.on_sleep = [&](int n) {
            if (n == 3) {
                kernel.SignalEvent(game_event);
            }
        };
        kernel.sleeps = 0;
        const int closes = kernel.closes.load();
        const ScanOutcome r = DoScan(*c.engine, p);
        CHECK_EQ(r.rc, 0u); // (cancelled: the scan still answers, with what it heard)
        CHECK_EQ(kernel.sleeps.load(), 3);
        CHECK_EQ(kernel.closes.load(), closes);
        kernel.on_sleep = nullptr;
        kernel.SignalEvent(game_event); // signalled before the scan starts
        kernel.sleeps = 0;
        CHECK_EQ(DoScan(*c.engine, p).rc, 0u);
        CHECK_EQ(kernel.sleeps.load(), 0);
        // a request for a ridiculous time stops at 10 s instead of holding the game
        p = ScanParams();
        p.channel_mask = 0x7FFF;
        p.dwell_ms = 0xFFFF;
        kernel.slept_ns = 0;
        CHECK_EQ(DoScan(*c.engine, p).rc, 0u);
        CHECK_EQ(kernel.slept_ns.load(), 10000000000LL);
    }

    // wlancommid / id8: enforced by default since 2026-10-20 (a network of another game is left out: NSMB2 listed the
    // emulator's Tekken lobby of the same room and joined it); the count-only mode is kept for the tests
    {
        ScanKernel kernel;
        Console c(room, 5, &kernel);
        CHECK_EQ(c.engine->Initialize(MakeNode(0x5555, "Filter", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        std::memset(&g_stats, 0, sizeof(g_stats));
        ScanParams p = NoWait();
        p.wlan_comm_id = 0x0016ED10; // another game's wlancommid: left out by default
        p.id8 = 1;
        refresh();
        CHECK_EQ(DoScan(*c.engine, p).entries, 0u);
        CHECK_EQ(g_stats.scan_id_differs, 1u);
        std::memset(&g_stats, 0, sizeof(g_stats));
        c.engine->SetScanFilterEnforced(false); // the count-only mode: nobody is left out
        p.wlan_comm_id = 0x0010CAFE;
        p.id8 = 1;
        refresh();
        CHECK_EQ(DoScan(*c.engine, p).entries, 1u);
        CHECK_EQ(g_stats.scan_id_differs, 0u);
        p.wlan_comm_id = 0x0016ED10;
        refresh();
        CHECK_EQ(DoScan(*c.engine, p).entries, 1u); // another game's wlancommid: still reported
        CHECK_EQ(g_stats.scan_id_differs, 1u);
        p.wlan_comm_id = 0xFFFFFFFF; // any game, but id8 7
        p.id8 = 7;
        refresh();
        CHECK_EQ(DoScan(*c.engine, p).entries, 1u);
        CHECK_EQ(g_stats.scan_id_differs, 2u);
        p.id8 = 0xFF; // any of both
        refresh();
        CHECK_EQ(DoScan(*c.engine, p).entries, 1u);
        CHECK_EQ(g_stats.scan_id_differs, 2u);
        CHECK_EQ(g_stats.scans, 4u);
        CHECK_EQ(g_stats.scan_entries, 4u);

        c.engine->SetScanFilterEnforced(true);
        p.wlan_comm_id = 0x0016ED10;
        refresh();
        CHECK_EQ(DoScan(*c.engine, p).entries, 0u);
        CHECK_EQ(g_stats.scan_id_differs, 3u);
        p.wlan_comm_id = 0x0010CAFE;
        refresh();
        CHECK_EQ(DoScan(*c.engine, p).entries, 1u);
        p.wlan_comm_id = 0xFFFFFFFF;
        p.id8 = 2;
        refresh();
        CHECK_EQ(DoScan(*c.engine, p).entries, 0u);
        c.engine->SetScanFilterEnforced(true); // (the default)

        // the BSSID filter keeps only that host
        p = NoWait();
        p.bssid = azhost.mac();
        refresh();
        CHECK_EQ(DoScan(*c.engine, p).entries, 1u);
        p.bssid = Mac{0x00, 0x1F, 0x32, 0x00, 0x00, 0x42};
        CHECK_EQ(DoScan(*c.engine, p).entries, 0u);

        // inputs the firmware would call invalid are counted, the scan runs all the same
        std::memset(&g_stats, 0, sizeof(g_stats));
        p = NoWait();
        p.dwell_ms = 5;
        CHECK_EQ(DoScan(*c.engine, p).rc, 0u);
        p = NoWait();
        p.mode = 9;
        CHECK_EQ(DoScan(*c.engine, p).rc, 0u);
        p.mode = 2;
        p.type = 2;
        CHECK_EQ(DoScan(*c.engine, p).rc, 0u);
        p.type = 1;
        p.ssid_len = 40;
        CHECK_EQ(DoScan(*c.engine, p).rc, 0u);
        CHECK_EQ(DoScan(*c.engine, NoWait()).rc, 0u);
        CHECK_EQ(g_stats.scan_odd_input, 4u);
        CHECK_EQ(g_stats.scans, 5u);
    }

    // a scan while connected hears the host that keeps beaconing, and the reply keeps the buffer size
    {
        ScanKernel kernel;
        Console c(room, 6, &kernel);
        CHECK_EQ(c.engine->Initialize(MakeNode(0x6666, "Joiner", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        refresh();
        NetworkInfo net;
        CHECK(FirstScannedNetwork(DoScan(*c.engine, NoWait()).buffer, &net));
        CHECK_EQ(c.engine->ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1), 0u);
        room.WaitIdle();
        refresh();
        std::vector<u8> buf(0x2000);
        CHECK_EQ(c.engine->ScanOnConnection(buf.data(), static_cast<u32>(buf.size()), NoWait()), 0u);
        CHECK_EQ(LoadLE32(buf.data() + 0), 0x2000u);
        CHECK_EQ(LoadLE32(buf.data() + 8), 1u);
        CHECK_EQ(c.engine->RecvBeaconBroadcastData(buf.data(), static_cast<u32>(buf.size()), NoWait()), kW); // (not idle)
    }
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------
// Stale nodes and the host's Deauthentication (spec F06 a-c, C5)
// ---------------------------------------------------------------------------------------------
namespace {

/// A client driven by hand: sends the frames of a join one at a time and keeps what comes back.
class RawClient {
public:
    RawClient(MockRoom& room, u8 mac_last) : mac{0x00, 0x1F, 0x32, 0x00, 0x00, mac_last}, room_(room) {
        index_ = room.AddMember(mac, [this](const WifiPacket& p) {
            std::lock_guard<std::mutex> l(m_);
            inbox_.push_back(p);
        });
    }
    ~RawClient() {
        room_.RemoveMember(index_);
    }
    void Send(WifiPacket::Type type, const Mac& to, std::vector<u8> data) {
        WifiPacket p;
        p.type = type;
        p.channel = 11;
        p.transmitter_address = mac;
        p.destination_address = to;
        p.data = std::move(data);
        g_rules.Inspect(p);
        room_.Send(index_, p);
    }
    void Auth(const Mac& host) {
        Send(WifiPacket::Type::Authentication, host, GenerateAuthenticationFrame(AuthSeq::Seq1));
    }
    void Start(const Mac& host) {
        Send(WifiPacket::Type::Data, host, GenerateEapolStartFrame(1, ConnectionType::Client, MakeNode(0x7000 + mac[5], "Raw", 0)));
    }
    void Deauth(const Mac& host) {
        Send(WifiPacket::Type::Deauthentication, host, {});
    }
    /// What arrived since the last Take().
    std::vector<WifiPacket> Take() {
        std::lock_guard<std::mutex> l(m_);
        std::vector<WifiPacket> out;
        out.swap(inbox_);
        return out;
    }
    int Count(WifiPacket::Type type) {
        std::lock_guard<std::mutex> l(m_);
        int n = 0;
        for (const WifiPacket& p : inbox_) {
            n += p.type == type;
        }
        return n;
    }
    /// EAPoL-Logoffs received, to everybody (`broadcast`) or to this client only.
    int Logoffs(bool broadcast) {
        std::lock_guard<std::mutex> l(m_);
        int n = 0;
        for (const WifiPacket& p : inbox_) {
            u16 magic = 0;
            EtherType type;
            if (p.type == WifiPacket::Type::Data && GetFrameEtherType(p.data, &type) && type == EtherType::EAPoL &&
                GetEapolFrameType(p.data, &magic) && magic == kEapolLogoffMagic) {
                n += (p.destination_address == kBroadcastMac) == broadcast;
            }
        }
        return n;
    }
    Mac mac;

private:
    MockRoom& room_;
    int index_ = -1;
    std::mutex m_;
    std::vector<WifiPacket> inbox_;
};

WifiPacket DeauthFrom(const Mac& from, const Mac& to) {
    WifiPacket p;
    p.type = WifiPacket::Type::Deauthentication;
    p.channel = 11;
    p.transmitter_address = from;
    p.destination_address = to;
    return p;
}

bool WaitForStatus(Bench& b, NetworkStatus status) {
    for (int i = 0; i < 2000; ++i) {
        if (b.e().status() == status) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

} // namespace

static void TestHostReauth() {
    std::printf("host: Authentication again, a repeated EAPoL-Start, a full network\n");
    g_rules.Reset();
    MockRoom room;
    Bench host(room, 1);
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(host.BeginHosting(2), 0u); // the host and one client: full with one
    host.Status();                       // (reads the join of the host)
    RawClient a(room, 2), b(room, 3);
    const Mac H = host.console.mac;

    // Authentication twice while half open: answered each time (a client that gave up and tries again)
    a.Auth(H);
    room.WaitIdle();
    CHECK_EQ(a.Count(WifiPacket::Type::Authentication), 1);
    CHECK_EQ(a.Count(WifiPacket::Type::AssociationResponse), 1);
    a.Auth(H);
    room.WaitIdle();
    CHECK_EQ(a.Count(WifiPacket::Type::Authentication), 2);
    CHECK_EQ(a.Count(WifiPacket::Type::AssociationResponse), 2);
    CHECK_EQ(host.Status().total_nodes, 1); // nothing admitted yet

    // the Start admits it as node 2, with the broadcast Logoff of a first admission
    a.Start(H);
    room.WaitIdle();
    CHECK_EQ(a.Logoffs(true), 1);
    ConnectionStatus st = host.Status();
    CHECK_EQ(st.total_nodes, 2);
    CHECK_EQ(st.node_bitmask, 3);
    const u32 events = host.Signals(host.status_event);

    // the network is full now: another client's Authentication is not answered
    b.Auth(H);
    room.WaitIdle();
    CHECK_EQ(b.Count(WifiPacket::Type::Authentication), 0);

    // the admitted client asks again with a Start: the same answer, to it alone, nothing changes
    a.Take();
    b.Take();
    host.sent.Clear();
    a.Start(H);
    room.WaitIdle();
    CHECK_EQ(a.Logoffs(false), 1);
    CHECK_EQ(a.Logoffs(true), 0);
    CHECK_EQ(b.Logoffs(true) + b.Logoffs(false), 0); // nobody else saw it
    CHECK_EQ(host.sent.CountTo(WifiPacket::Type::Data, a.mac), 1);
    CHECK_EQ(host.Signals(host.status_event), events);
    EapolLogoff logoff;
    const std::vector<WifiPacket> got = a.Take();
    CHECK_EQ(got.size(), std::size_t(1));
    CHECK(got.size() == 1 && ParseEapolLogoffFrame(got[0].data, &logoff));
    CHECK_EQ(logoff.assigned_node_id, 2);
    CHECK_EQ(logoff.connected_nodes, 2);

    // it drops without a word and comes back: the new association replaces the old one although the
    // network is full, the node is gone for the game until the Start brings it back, as node 2 again
    a.Auth(H);
    room.WaitIdle();
    CHECK_EQ(a.Count(WifiPacket::Type::Authentication), 1);
    CHECK_EQ(a.Count(WifiPacket::Type::AssociationResponse), 1);
    st = host.Status();
    CHECK_EQ(st.total_nodes, 1);
    CHECK_EQ(st.node_bitmask, 1);
    CHECK_EQ(st.changed_nodes, 2);
    CHECK_EQ(host.Signals(host.status_event), events + 1);
    a.Take();
    a.Start(H);
    room.WaitIdle();
    CHECK_EQ(a.Logoffs(true), 1); // the first admission of the new association
    const std::vector<WifiPacket> again = a.Take();
    bool admitted = false;
    for (const WifiPacket& p : again) {
        u16 magic = 0;
        if (p.type == WifiPacket::Type::Data && GetEapolFrameType(p.data, &magic) && magic == kEapolLogoffMagic &&
            ParseEapolLogoffFrame(p.data, &logoff)) {
            admitted = logoff.assigned_node_id == 2 && logoff.connected_nodes == 2;
        }
    }
    CHECK(admitted);
    st = host.Status();
    CHECK_EQ(st.total_nodes, 2);
    CHECK_EQ(st.node_bitmask, 3);
    CHECK_EQ(host.Signals(host.status_event), events + 2);
    // still full for everybody else
    b.Take();
    b.Auth(H);
    room.WaitIdle();
    CHECK_EQ(b.Count(WifiPacket::Type::Authentication), 0);
    CHECK_RULES();
}

// A client that is still Connecting takes any EAPoL-Logoff it sees for its own admission (Azahar does):
// the answer to a repeated Start must not be a broadcast.
static void TestRepeatedStartDoesNotAdmitOthers() {
    std::printf("host: the answer to a repeated Start is not adopted by a client that is joining\n");
    g_rules.Reset();
    MockRoom room;
    Bench host(room, 1), carol(room, 4);
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(carol.Init(), 0u);
    CHECK_EQ(host.BeginHosting(4), 0u);
    RawClient a(room, 2);
    const Mac H = host.console.mac;
    a.Auth(H);
    a.Start(H);
    room.WaitIdle();
    CHECK_EQ(host.Status().total_nodes, 2);

    // carol joins a host that never answers (the test kernel waits 0.3 s instead of 5)
    NetworkInfo nobody = MakeNetworkInfo(4, 0);
    nobody.host_mac_address = Mac{0x00, 0x1F, 0x32, 0x00, 0x00, 0x55};
    nobody.channel = 11;
    u32 result = 0;
    std::thread joining([&] { result = carol.e().ConnectToNetwork(reinterpret_cast<u8*>(&nobody), sizeof(nobody), 1); });
    CHECK(WaitForStatus(carol, NetworkStatus::Connecting));
    a.Start(H);
    room.WaitIdle();
    CHECK(carol.e().status() == NetworkStatus::Connecting); // she did not take a's answer for hers
    joining.join();
    CHECK_EQ(result, kConnectTimeout);
    CHECK_EQ(carol.Status().total_nodes, 0);
    CHECK_RULES();
}

// nwm looks for the same MAC before it looks at the lobby's attributes: a client that was admitted and
// asks again (its answer was lost) is answered although the lobby is closed since, and nobody else is.
static void TestRepeatedStartInClosedLobby() {
    std::printf("host: a closed lobby still answers a repeated Start of an admitted client, and nobody else\n");
    g_rules.Reset();
    MockRoom room;
    Bench host(room, 1);
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(host.BeginHosting(4), 0u);
    RawClient a(room, 2), b(room, 3), stranger(room, 4);
    const Mac H = host.console.mac;
    a.Auth(H);
    b.Auth(H); // half open: it never sends its Start before the lobby closes
    room.WaitIdle();
    a.Start(H);
    room.WaitIdle();
    CHECK_EQ(host.Status().total_nodes, 2);
    const u32 events = host.Signals(host.status_event);

    // 2 = closed to new clients, 4 = closed to everybody (a returning client included)
    for (const u16 mask : {u16(2), u16(4)}) {
        CHECK_EQ(host.e().UpdateNetworkAttribute(mask, 1), 0u);
        a.Take();
        b.Take();
        stranger.Take();
        host.sent.Clear();
        a.Start(H);        // admitted before: its answer again, to it alone
        b.Start(H);        // authenticated, never admitted: refused (silently: Azahar cannot read a refusal)
        stranger.Start(H); // never authenticated
        room.WaitIdle();
        CHECK_EQ(a.Logoffs(false), 1);
        CHECK_EQ(a.Logoffs(true), 0);
        CHECK_EQ(host.sent.CountTo(WifiPacket::Type::Data, a.mac), 1);
        CHECK_EQ(b.Logoffs(true) + b.Logoffs(false), 0);
        CHECK_EQ(stranger.Logoffs(true) + stranger.Logoffs(false), 0);
        CHECK_EQ(host.sent.Count(WifiPacket::Type::Data), 1); // nothing else on the wire
        CHECK_EQ(host.Status().total_nodes, 2);
        CHECK_EQ(host.Signals(host.status_event), events);
        CHECK_EQ(host.e().UpdateNetworkAttribute(mask, 0), 0u); // open again
    }
    // and open again, the half-open client is admitted by its Start as usual
    b.Take();
    b.Start(H);
    room.WaitIdle();
    CHECK_EQ(b.Logoffs(true), 1);
    CHECK_EQ(host.Status().total_nodes, 3);
    CHECK_RULES();
}

static void TestClientHonoursHostDeauth() {
    std::printf("client: leaves when its host sends the Deauthentication, ignores anybody else's\n");
    g_rules.Reset();
    MockRoom room;
    Bench host(room, 1), alice(room, 2), bob(room, 3), carol(room, 4);
    for (Bench* b : {&host, &alice, &bob, &carol}) {
        CHECK_EQ(b->Init(), 0u);
    }
    CHECK_EQ(host.BeginHosting(4), 0u);
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    CHECK_EQ(JoinBench(room, bob, host), 0u);
    u32 bind_event = 0;
    CHECK_EQ(alice.e().Bind(2, 0x2E30, 0xF3, 0xFFFF, &bind_event), 0u);
    alice.Status();
    bob.Status();
    const u32 alice_events = alice.Signals(alice.status_event);
    const u32 bob_events = bob.Signals(bob.status_event);

    // a Deauthentication from a node that is not the host means nothing to a client
    alice.e().OnWifiPacket(DeauthFrom(bob.console.mac, kBroadcastMac));
    CHECK(alice.e().status() == NetworkStatus::ConnectedAsClient);
    CHECK_EQ(alice.Signals(alice.status_event), alice_events);

    // the host ends its network: its three Deauthentications take the clients out, once, with reason 4,
    // and they say nothing back
    alice.sent.Clear();
    bob.sent.Clear();
    CHECK_EQ(host.e().DestroyNetwork(), 0u);
    room.WaitIdle();
    for (Bench* b : {&alice, &bob}) {
        const ConnectionStatus st = b->Status();
        CHECK_EQ(st.status, 3u);
        CHECK_EQ(st.status_change_reason, 4u); // DISCARDED_FROM_NETWORK
        CHECK_EQ(st.total_nodes, 0);
        CHECK_EQ(b->sent.Total(), std::size_t(0));
    }
    CHECK_EQ(alice.Signals(alice.status_event), alice_events + 1);
    CHECK_EQ(bob.Signals(bob.status_event), bob_events + 1);
    CHECK_EQ(alice.Signals(bind_event), 1u);
    u16 src = 0;
    u32 size = 0;
    std::vector<u8> out;
    CHECK_EQ(PullInto(alice.e(), 2, &out, &size, &src), kW);

    // A join in progress: a Deauthentication from another address is not the host's, and one from the
    // host before it has answered is a stale one (the burst of a network that ended a moment ago may
    // still be on its way); the join goes on and times out. Once the host has answered, a
    // Deauthentication from it ends the join at once (0xC92113FB, not the timeout 0xC92113FE).
    NetworkInfo gone = MakeNetworkInfo(4, 0);
    gone.host_mac_address = Mac{0x00, 0x1F, 0x32, 0x00, 0x00, 0x55};
    gone.channel = 11;
    auto from_host = [&](WifiPacket::Type type, std::vector<u8> data) {
        WifiPacket p;
        p.type = type;
        p.channel = 11;
        p.transmitter_address = gone.host_mac_address;
        p.destination_address = carol.console.mac;
        p.data = std::move(data);
        carol.e().OnWifiPacket(p);
    };
    for (const bool answered : {false, true}) {
        u32 result = 0;
        std::thread joining([&] { result = carol.e().ConnectToNetwork(reinterpret_cast<u8*>(&gone), sizeof(gone), 1); });
        CHECK(WaitForStatus(carol, NetworkStatus::Connecting));
        carol.e().OnWifiPacket(DeauthFrom(Mac{0x00, 0x1F, 0x32, 0x00, 0x00, 0x66}, kBroadcastMac));
        CHECK(carol.e().status() == NetworkStatus::Connecting);
        if (answered) {
            from_host(WifiPacket::Type::Authentication, GenerateAuthenticationFrame(AuthSeq::Seq2));
            from_host(WifiPacket::Type::AssociationResponse, GenerateAssocResponseFrame(0, 1, 0xDEADBEEF));
        }
        carol.e().OnWifiPacket(DeauthFrom(gone.host_mac_address, carol.console.mac));
        if (!answered) {
            CHECK(carol.e().status() == NetworkStatus::Connecting);
        }
        joining.join();
        CHECK_EQ(result, answered ? 0xC92113FBu : kConnectTimeout);
        const ConnectionStatus st = carol.Status();
        CHECK_EQ(st.status, 3u);
        CHECK_EQ(st.status_change_reason, 0u);
    }
    CHECK_RULES();
}

// The role the other suites barely touch: a spectator joins an Azahar host, hears its data, cannot send,
// is thrown out by the host's Deauthentication (reason 4) and leaves with one Deauthentication of its own.
static void TestSpectatorLifecycle() {
    std::printf("spectator: joins, receives, cannot send, is ejected by the host, leaves\n");
    g_rules.Reset();
    MockRoom room;
    NetworkInfo ni = MakeNetworkInfo(4, 6);
    AzaharHost azhost(room, 1, ni, MakeNode(0x1111, "AzHost", 0));
    Bench spec(room, 2);
    CHECK_EQ(spec.Init(), 0u);
    u32 bind_event = 0;
    CHECK_EQ(spec.e().Bind(2, 0x2E30, 1, kBroadcastNetworkNodeId, &bind_event), 0u);
    azhost.SendBeacon();
    room.WaitIdle();
    NetworkInfo net;
    CHECK(spec.ScanFirst(&net));
    CHECK_EQ(spec.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 2), 0u);
    room.WaitIdle();
    ConnectionStatus st = spec.Status();
    CHECK_EQ(st.status, 10u);
    CHECK_EQ(st.status_change_reason, 1u); // NETWORK_IS_AVAILABLE

    // the host's frames for everybody reach a spectator, once
    const u8 msg[] = "to everybody";
    CHECK(azhost.SendData(0xFFFF, 1, 2, std::vector<u8>(msg, msg + sizeof(msg))));
    room.WaitIdle();
    std::vector<u8> out;
    u32 size = 0;
    u16 src = 0;
    CHECK_EQ(PullInto(spec.e(), 2, &out, &size, &src), 0u);
    CHECK_EQ(size, u32(sizeof(msg)));
    CHECK_EQ(src, 1);
    CHECK_EQ(PullInto(spec.e(), 2, &out, &size, &src), 0u);
    CHECK_EQ(size, 0u);
    // a spectator does not send: the firmware answers "wrong state" to SendTo
    CHECK_EQ(spec.e().SendTo(1, 1, sizeof(msg), 0, msg, sizeof(msg)), kW);

    // the host throws it out
    WifiPacket deauth;
    deauth.type = WifiPacket::Type::Deauthentication;
    deauth.channel = 11;
    deauth.transmitter_address = azhost.mac();
    deauth.destination_address = kBroadcastMac;
    const u32 events = spec.Signals(spec.status_event);
    spec.sent.Clear();
    spec.e().OnWifiPacket(deauth);
    st = spec.Status();
    CHECK_EQ(st.status, 3u);
    CHECK_EQ(st.status_change_reason, 4u); // DISCARDED_FROM_NETWORK
    CHECK_EQ(spec.Signals(spec.status_event), events + 1);
    CHECK_EQ(spec.sent.Total(), std::size_t(0)); // it says nothing back

    // joins again and leaves politely: one Deauthentication to the host
    azhost.SendBeacon();
    room.WaitIdle();
    CHECK(spec.ScanFirst(&net));
    CHECK_EQ(spec.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 2), 0u);
    room.WaitIdle();
    const u32 departures = azhost.departures();
    CHECK_EQ(spec.e().DisconnectNetwork(), 0u);
    room.WaitIdle();
    CHECK_EQ(azhost.departures(), departures + 1);
    st = spec.Status();
    CHECK_EQ(st.status, 3u);
    CHECK_EQ(st.status_change_reason, 2u); // REQUEST_FROM_MYSELF
    CHECK_RULES();
}

static void TestPreDeauth() {
    std::printf("client: starts over with a Deauthentication to a host it has heard\n");
    g_rules.Reset();
    MockRoom room;
    NetworkInfo ni = MakeNetworkInfo(2, 4); // the host and one client: a stale node blocks the only slot
    AzaharHost azhost(room, 1, ni, MakeNode(0x1111, "AzHost", 0));
    {
        // a console that was admitted and vanished (a restarted game): the host never hears a goodbye
        RawClient ghost(room, 2);
        ghost.Auth(azhost.mac());
        ghost.Start(azhost.mac());
        room.WaitIdle();
        CHECK_EQ(azhost.total(), 2);
    }
    Bench alice(room, 2); // the same console (same MAC) comes back
    CHECK_EQ(alice.Init(), 0u);
    azhost.SendBeacon();
    room.WaitIdle();
    NetworkInfo net;
    CHECK(alice.ScanFirst(&net));
    alice.sent.Clear();
    CHECK_EQ(alice.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1), 0u); // not a timeout
    room.WaitIdle();
    const std::vector<WifiPacket> sent = alice.sent.All();
    CHECK(sent.size() >= 3);
    if (sent.size() >= 3) {
        CHECK(sent[0].type == WifiPacket::Type::Deauthentication && sent[0].destination_address == azhost.mac());
        CHECK(sent[1].type == WifiPacket::Type::Authentication && sent[1].destination_address == azhost.mac());
    }
    CHECK_EQ(azhost.departures(), 1u); // the ghost went
    CHECK_EQ(azhost.total(), 2);       // and the console is in
    CHECK_EQ(alice.Status().network_node_id, 2);
    CHECK_EQ(alice.e().DisconnectNetwork(), 0u);
    room.WaitIdle();

    // an address nobody scanned gets no such frame
    Bench bob(room, 3);
    CHECK_EQ(bob.Init(), 0u);
    NetworkInfo unscanned = MakeNetworkInfo(4, 0);
    unscanned.host_mac_address = Mac{0x00, 0x1F, 0x32, 0x00, 0x00, 0x55};
    unscanned.channel = 11;
    CHECK_EQ(bob.e().ConnectToNetwork(reinterpret_cast<u8*>(&unscanned), sizeof(unscanned), 1), kConnectTimeout);
    CHECK_EQ(bob.sent.Count(WifiPacket::Type::Deauthentication), 0);
    CHECK_EQ(bob.sent.Count(WifiPacket::Type::Authentication), 1);
    CHECK_RULES();

    // the rule checker knows the difference: a Deauthentication and an Authentication to a MAC whose
    // beacon it never saw is a guessed address
    AzaharRules r;
    const Mac from{0, 0x1F, 0x32, 0, 0, 9}, to{0, 0x1F, 0x32, 0, 0, 8};
    r.AddMember(to);
    r.Inspect(MakePkt(WifiPacket::Type::Deauthentication, from, to, {}));
    r.Inspect(MakePkt(WifiPacket::Type::Authentication, from, to, GenerateAuthenticationFrame(AuthSeq::Seq1)));
    CHECK(r.Has("pre-deauth-unscanned"));
    AzaharRules heard;
    heard.AddMember(to);
    heard.Inspect(MakePkt(WifiPacket::Type::Beacon, to, kBroadcastMac, {}));
    heard.Inspect(MakePkt(WifiPacket::Type::Deauthentication, from, to, {}));
    heard.Inspect(MakePkt(WifiPacket::Type::Authentication, from, to, GenerateAuthenticationFrame(AuthSeq::Seq1)));
    CHECK_EQ(heard.count(), std::size_t(0));
    // ... but one it spoke to before is no guess: leaving and joining again is Deauthentication, Authentication
    AzaharRules spoke;
    spoke.AddMember(to);
    spoke.Inspect(MakePkt(WifiPacket::Type::Authentication, from, to, GenerateAuthenticationFrame(AuthSeq::Seq1)));
    spoke.Inspect(MakePkt(WifiPacket::Type::Deauthentication, from, to, {}));
    spoke.Inspect(MakePkt(WifiPacket::Type::Authentication, from, to, GenerateAuthenticationFrame(AuthSeq::Seq1)));
    CHECK_EQ(spoke.count(), std::size_t(0));
}

// F05: a host re-sends a frame for another node only if it was addressed to the host's own MAC
static void TestHostForwardRule() {
    std::printf("host: forwards a client's frame for another node only if it was sent to the host's MAC\n");
    g_rules.Reset();
    MockRoom room;
    Bench host(room, 1), alice(room, 2), bob(room, 3);
    for (Bench* b : {&host, &alice, &bob}) {
        CHECK_EQ(b->Init(), 0u);
    }
    CHECK_EQ(host.BeginHosting(4), 0u);
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    CHECK_EQ(JoinBench(room, bob, host), 0u);
    u32 bob_bind = 0;
    CHECK_EQ(bob.e().Bind(2, 0x2E30, 1, 0xFFFF, &bob_bind), 0u);
    const u8 msg[] = "for node 3";
    u16 src = 0;
    u32 size = 0;
    auto bob_copies = [&] {
        int n = 0;
        for (;;) {
            PullAll(bob.console, 2, &src, &size);
            if (size == 0) {
                return n;
            }
            CHECK_EQ(src, 2);
            ++n;
        }
    };

    // the broadcast flag (FFE): the room hands bob the frame, the host has nothing to add
    std::memset(&g_stats, 0, sizeof(g_stats));
    host.sent.Clear();
    CHECK_EQ(alice.e().SendTo(3, 1, sizeof(msg), 2, msg, sizeof(msg)), 0u);
    room.WaitIdle();
    CHECK_EQ(host.sent.CountSecureData(), 0);
    CHECK_EQ(g_stats.forwarded, 0u);
    CHECK_EQ(g_stats.drop_not_for_me, 1u);
    CHECK_EQ(bob_copies(), 1);

    // bob's MAC: it goes to him alone
    CHECK_EQ(alice.e().SendTo(3, 1, sizeof(msg), 0, msg, sizeof(msg)), 0u);
    room.WaitIdle();
    CHECK_EQ(host.sent.CountSecureData(), 0);
    CHECK_EQ(bob_copies(), 1);

    // addressed to the host's MAC for node 3 (what a client sends when it does not know bob's MAC):
    // the host relays it, once. (The bytes differ from the frames above: the twin filter takes the relayed copy
    // of a frame that equals one it delivered a moment ago for the twin of that one.)
    const u8 via_host[] = "for node 3, by the host";
    WifiPacket p;
    p.type = WifiPacket::Type::Data;
    p.channel = 11;
    p.transmitter_address = alice.console.mac;
    p.destination_address = host.console.mac;
    p.data = GenerateDataPayload(via_host, sizeof(via_host), 1, 3, 2, 0);
    host.e().OnWifiPacket(p);
    room.WaitIdle();
    CHECK_EQ(host.sent.CountSecureData(), 1);
    CHECK_EQ(g_stats.forwarded, 1u);
    CHECK_EQ(bob_copies(), 1);

    // ... which is what alice's SendTo does for a node in her status block whose MAC she lacks
    WifiPacket empty_map;
    empty_map.type = WifiPacket::Type::NodeMap;
    empty_map.channel = 11;
    empty_map.transmitter_address = host.console.mac;
    empty_map.destination_address = kBroadcastMac;
    empty_map.data = GenerateNodeMapPacket({});
    alice.e().OnWifiPacket(empty_map);
    CHECK((alice.Status().node_bitmask & 4) != 0);
    host.sent.Clear();
    CHECK_EQ(alice.e().SendTo(3, 1, sizeof(via_host), 0, via_host, sizeof(via_host)), 0u);
    room.WaitIdle();
    CHECK_EQ(host.sent.CountSecureData(), 1);
    CHECK_EQ(bob_copies(), 1);

    // frames for the host and for everybody are not routed either
    host.sent.Clear();
    CHECK_EQ(alice.e().SendTo(1, 1, sizeof(msg), 0, msg, sizeof(msg)), 0u);
    CHECK_EQ(alice.e().SendTo(kBroadcastNetworkNodeId, 1, sizeof(msg), 0, msg, sizeof(msg)), 0u);
    room.WaitIdle();
    CHECK_EQ(host.sent.CountSecureData(), 0);
    CHECK_EQ(bob_copies(), 1); // the broadcast node one
    CHECK_RULES();
}

// F14 in shadow: the node set the host's beacons announce is compared with the one the Logoff frames
// built; differences are counted and NOTHING else happens.
static void TestBeaconSyncShadow() {
    std::printf("beacon sync (shadow): counts the differences, changes nothing\n");
    g_rules.Reset();
    MockRoom room;
    NetworkInfo ni = MakeNetworkInfo(4, 6);
    AzaharHost azhost(room, 1, ni, MakeNode(0x1111, "AzHost", 0));
    RawClient spy(room, 9); // hears every broadcast: keeps the beacons
    Bench alice(room, 2);
    CHECK_EQ(alice.Init(), 0u);
    const Mac H = azhost.mac();
    azhost.SendBeacon();
    room.WaitIdle();
    NetworkInfo net;
    CHECK(alice.ScanFirst(&net));
    CHECK_EQ(alice.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1), 0u);
    room.WaitIdle();
    auto last_beacon = [&] {
        WifiPacket last;
        for (const WifiPacket& p : spy.Take()) {
            if (p.type == WifiPacket::Type::Beacon) {
                last = p;
            }
        }
        return last;
    };
    spy.Take();
    azhost.SendBeacon(); // built before bob joins
    room.WaitIdle();
    const WifiPacket before_bob = last_beacon();
    CHECK(before_bob.type == WifiPacket::Type::Beacon);

    RawClient bob(room, 3);
    bob.Auth(H);
    bob.Start(H);
    room.WaitIdle();
    CHECK_EQ(alice.Status().node_bitmask, 7); // the Logoff told alice about bob

    // beacons that list what the Logoff frames built: no difference
    std::memset(&g_stats, 0, sizeof(g_stats));
    azhost.SendBeacon();
    azhost.SendBeacon();
    room.WaitIdle();
    CHECK_EQ(g_stats.sync_checks, 2u);
    CHECK_EQ(g_stats.sync_disagree, 0u);

    // a beacon that was built before the join and arrives after the Logoff: one difference, then none
    alice.e().OnWifiPacket(before_bob);
    CHECK_EQ(g_stats.sync_checks, 3u);
    CHECK_EQ(g_stats.sync_disagree, 1u);
    azhost.SendBeacon();
    room.WaitIdle();
    CHECK_EQ(g_stats.sync_disagree, 1u);
    CHECK_EQ(g_stats.sync_streak_max, 1u);

    // bob leaves with a Deauthentication; an Azahar host tells nobody. The host's beacons say so, the
    // Logoff-derived set does not: three differences in a row, and the game sees nothing change.
    const u32 events = alice.Signals(alice.status_event);
    alice.sent.Clear();
    const ConnectionStatus before = alice.Status();
    bob.Deauth(H);
    room.WaitIdle();
    std::memset(&g_stats, 0, sizeof(g_stats));
    for (int i = 0; i < 3; ++i) {
        azhost.SendBeacon();
    }
    room.WaitIdle();
    CHECK_EQ(g_stats.sync_checks, 3u);
    CHECK_EQ(g_stats.sync_disagree, 3u);
    CHECK_EQ(g_stats.sync_streak_max, 3u);
    ConnectionStatus after = alice.Status();
    CHECK_EQ(after.status, 9u);
    CHECK_EQ(after.node_bitmask, before.node_bitmask);
    CHECK_EQ(after.total_nodes, before.total_nodes);
    CHECK_EQ(after.status_change_reason, before.status_change_reason);
    CHECK_EQ(after.changed_nodes, 0);
    CHECK_EQ(alice.Signals(alice.status_event), events);
    CHECK_EQ(alice.sent.Total(), std::size_t(0));

    // a beacon that leaves out alice's own node is counted too, and she stays where she is
    std::vector<NodeInfo> only_host(4);
    only_host[0] = MakeNode(0x1111, "AzHost", 1);
    WifiPacket without_me = before_bob;
    without_me.data = GenerateBeaconFrame(azhost.network(), only_host);
    std::memset(&g_stats, 0, sizeof(g_stats));
    alice.e().OnWifiPacket(without_me);
    CHECK_EQ(g_stats.sync_disagree, 1u);
    CHECK(alice.e().status() == NetworkStatus::ConnectedAsClient);
    CHECK_EQ(alice.Signals(alice.status_event), events);

    // a beacon that is not of this format (cut short, another key) is not compared
    std::memset(&g_stats, 0, sizeof(g_stats));
    WifiPacket garbage = before_bob;
    garbage.data.resize(40);
    alice.e().OnWifiPacket(garbage);
    WifiPacket other_key = before_bob;
    other_key.data[other_key.data.size() - 5] ^= 0xFF; // the node list no longer matches its MD5
    alice.e().OnWifiPacket(other_key);
    CHECK_EQ(g_stats.sync_checks, 0u);
    // and a beacon of somebody else is not the host's
    WifiPacket stranger = before_bob;
    stranger.transmitter_address = Mac{0x00, 0x1F, 0x32, 0x00, 0x00, 0x77};
    alice.e().OnWifiPacket(stranger);
    CHECK_EQ(g_stats.sync_checks, 0u);
    CHECK_RULES();
}

// The fixed ways a command refuses things when the engine has never been told to start (S0)
static void TestEngineBeforeInitialize() {
    std::printf("engine before Initialize\n");
    MockRoom room;
    Bench a(room, 1);
    u8 channel;
    NodeInfo node;
    u32 event;
    u16 src;
    u32 size;
    std::vector<u8> out;
    CHECK_EQ(a.e().BeginHostingNetwork(nullptr, 0), kW);
    CHECK_EQ(a.e().GetNodeInformation(1, &node), kW);
    CHECK_EQ(a.e().GetChannel(&channel), kW);
    CHECK_EQ(a.e().Bind(2, 0x2E30, 1, 0xFFFF, &event), kW);
    CHECK_EQ(a.e().Unbind(2), 0u);
    CHECK_EQ(PullInto(a.e(), 2, &out, &size, &src), kW);
    CHECK_EQ(a.e().SendTo(1, 1, 0, 0, nullptr, 0), kW);
    CHECK_EQ(a.e().Shutdown(), 0u);
    ConnectionStatus st = a.Status();
    CHECK_EQ(st.status, 0u);
    CHECK_EQ(st.status_change_reason, 0u);
    CHECK_EQ(st.network_node_id, 0);
}

// ---------------------------------------------------------------------------------------------
// CPU and instrumentation (spec F20, F21 and the plugin-cost items): the clock conversion, the bounded
// TX queue, the lock counters, the statistics sinks, the early drop of frames that are not for us
// ---------------------------------------------------------------------------------------------
static void TestTicks() {
    std::printf("ticks: milliseconds and microseconds without a 64-bit division\n");
    const auto exact_ms = [](u64 t) { return static_cast<u64>(static_cast<unsigned __int128>(t) * 1000 / 268111856u); };
    const auto exact_us = [](u64 t) { return static_cast<u64>(static_cast<unsigned __int128>(t) * 1000000 / 268111856u); };
    const auto diff = [](u64 a, u64 b) { return a > b ? a - b : b - a; };
    // the conversion has a relative error below 5e-10 (the constants are rounded to 32 bits), and the
    // result is floored: at most one off plus that
    const auto close = [&diff](u64 a, u64 exact) { return diff(a, exact) * 1000000000ull <= exact + 1000000000ull; };
    // the boundaries of a millisecond, a second and a day
    for (u64 t : {u64(0), u64(1), u64(268), u64(268111), u64(268112), u64(268113), u64(268111856), u64(268111855),
                  u64(268111857), u64(268111856) * 3600, u64(268111856) * 86400}) {
        CHECK(close(azl_ticks_to_ms(t), exact_ms(t)));
        CHECK(close(azl_ticks_to_us(t), exact_us(t)));
    }
    // an xorshift walk over every magnitude the clock has in its first 12 days (2^48 ticks)
    u64 x = 88172645463325252ull;
    for (int i = 0; i < 40000; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        const u64 t = x >> (16 + (i % 36)); // magnitudes from 2^13 to 2^48
        CHECK(close(azl_ticks_to_ms(t), exact_ms(t)));
        CHECK(close(azl_ticks_to_us(t), exact_us(t)));
    }
    // beyond that the error is relative: below 1e-9 (plus one) up to 2^62 ticks, years of uptime
    for (int i = 0; i < 20000; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        const u64 t = (x >> 2) | (1ull << 49);
        const u64 e = exact_ms(t);
        CHECK(close(azl_ticks_to_ms(t), e));
    }
    // monotonic: a later tick is never an earlier millisecond
    u64 last = 0;
    for (u64 t = 0; t < 268111856ull * 2; t += 12345) {
        const u64 ms = azl_ticks_to_ms(t);
        CHECK(ms >= last);
        last = ms;
    }
    CHECK_EQ(AZL_TICKS_PER_MS, 268112u);
    // the platform clock agrees with the tick clock
    const u64 t0 = platform::NowTicks();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const u64 dt_ms = azl_ticks_to_ms(platform::NowTicks() - t0);
    CHECK(dt_ms >= 28 && dt_ms < 500);
    // ... and so do the platform's milliseconds and microseconds: code that reads one and the other (the network
    // loop starts from NowMs() and goes on with ticks) must never see a later reading come out earlier. A clock that
    // truncated to milliseconds on its own was a fraction of a millisecond behind the ticks at any uptime, and a
    // pause of 49 days was measured in one run of a few dozen.
    int backwards = 0;
    for (int i = 0; i < 200000; ++i) {
        const u64 ms = platform::NowMs();
        const u64 us = platform::NowUs();
        backwards += azl_ticks_to_ms(platform::NowTicks()) < ms;
        backwards += azl_ticks_to_us(platform::NowTicks()) < us;
    }
    CHECK_EQ(backwards, 0);
}

static TxQueue::Item TxItem(u32 size, u64 ms, bool expendable, u8 tag = 0, bool reliable = true) {
    TxQueue::Item item;
    item.bytes.assign(size, tag);
    item.queued_ms = ms;
    item.expendable = expendable;
    item.reliable = reliable;
    return item;
}

static std::vector<u8> TxTags(TxQueue& q, u64 now_ms, TxQueue::Drops* drops) {
    std::deque<TxQueue::Item> out;
    q.Take(&out, now_ms, drops);
    std::vector<u8> tags;
    for (const auto& item : out) {
        tags.push_back(item.bytes.empty() ? 0 : item.bytes[0]);
    }
    return tags;
}

static void TestTxQueue() {
    std::printf("tx queue: bounded, oldest game frame first, handshake protected\n");
    // frames: 128 at most, the oldest goes
    {
        TxQueue q;
        TxQueue::Drops d;
        for (int i = 0; i < 200; ++i) {
            CHECK(q.Push(TxItem(100, 1000, true, static_cast<u8>(i)), 1000, &d));
        }
        CHECK_EQ(q.size(), std::size_t(128));
        CHECK_EQ(q.bytes(), std::size_t(128 * 100));
        CHECK_EQ(d.cap, 72u);
        CHECK_EQ(d.stale, 0u);
        const auto tags = TxTags(q, 1000, &d);
        CHECK_EQ(tags.size(), std::size_t(128));
        for (std::size_t i = 0; i < tags.size(); ++i) {
            CHECK_EQ(tags[i], static_cast<u8>(72 + i)); // the newest 128, in order
        }
        CHECK(q.empty() && q.bytes() == 0);
    }
    // bytes: 32,768 at most (a frame that does not fit pushes out as many old ones as it needs)
    {
        TxQueue q;
        TxQueue::Drops d;
        for (int i = 0; i < 100; ++i) {
            CHECK(q.Push(TxItem(400, 5, true, static_cast<u8>(i)), 5, &d));
        }
        CHECK(q.bytes() <= 32768 && q.size() == 81); // 81 * 400 = 32,400
        CHECK_EQ(d.cap, 19u);
        CHECK(q.Push(TxItem(1600, 5, true, 200), 5, &d)); // needs 1,200 more bytes than are left: 4 old ones go
        CHECK(q.bytes() <= 32768);
        CHECK_EQ(d.cap, 19u + 4u);
        const auto tags = TxTags(q, 5, &d);
        CHECK_EQ(tags.back(), 200);
        CHECK_EQ(tags.front(), 23); // frames 0..18 went for the cap, 19..22 for the big one
        // one frame bigger than the whole cap is refused and costs nothing else
        TxQueue q2;
        TxQueue::Drops d2;
        CHECK(q2.Push(TxItem(100, 0, true, 1), 0, &d2));
        CHECK(!q2.Push(TxItem(32769, 0, true, 2), 0, &d2));
        CHECK_EQ(q2.size(), std::size_t(1));
        CHECK_EQ(d2.cap, 1u);
        CHECK(q2.Push(TxItem(32768, 0, true, 3), 0, &d2)); // exactly the cap: the small one makes room
        CHECK_EQ(q2.size(), std::size_t(1));
        CHECK_EQ(d2.cap, 2u);
    }
    // age: more than a second is stale, a second is not; the handshake is never stale
    {
        TxQueue q;
        TxQueue::Drops d;
        q.Push(TxItem(50, 0, true, 1), 0, &d);
        q.Push(TxItem(50, 0, false, 2), 0, &d); // handshake
        q.Push(TxItem(50, 500, true, 3), 500, &d);
        q.Push(TxItem(50, 1000, true, 4), 1000, &d);
        auto tags = TxTags(q, 1000, &d); // 1000 ms old: not stale yet
        CHECK_EQ(tags.size(), std::size_t(4));
        CHECK_EQ(d.stale, 0u);
        q.Push(TxItem(50, 0, true, 1), 0, &d);
        q.Push(TxItem(50, 0, false, 2), 0, &d);
        q.Push(TxItem(50, 500, true, 3), 500, &d);
        q.Push(TxItem(50, 1000, true, 4), 1000, &d);
        tags = TxTags(q, 1001, &d); // 1001 ms
        CHECK_EQ(tags.size(), std::size_t(3));
        CHECK(tags[0] == 2 && tags[1] == 3 && tags[2] == 4);
        CHECK_EQ(d.stale, 1u);
        q.Push(TxItem(50, 0, true, 1), 0, &d);
        q.Push(TxItem(50, 0, false, 2), 0, &d);
        tags = TxTags(q, 100000, &d); // a stall of 100 s: only the handshake is left
        CHECK(tags.size() == 1 && tags[0] == 2);
        CHECK_EQ(d.stale, 2u);
        // stale frames are also dropped when a new one is queued, before a fresh one is pushed out
        TxQueue q2;
        TxQueue::Drops d2;
        for (int i = 0; i < 128; ++i) {
            q2.Push(TxItem(40, 0, true, static_cast<u8>(i)), 0, &d2);
        }
        CHECK(q2.Push(TxItem(40, 2000, true, 250), 2000, &d2)); // everything before is stale
        CHECK_EQ(d2.stale, 128u);
        CHECK_EQ(d2.cap, 0u);
        CHECK_EQ(q2.size(), std::size_t(1));
    }
    // the handshake: its own cap of 32 (the newest is refused), order kept among game frames, join first
    {
        TxQueue q;
        TxQueue::Drops d;
        for (int i = 0; i < 32; ++i) {
            CHECK(q.Push(TxItem(60, 0, false, static_cast<u8>(100 + i)), 0, &d));
        }
        CHECK(!q.Push(TxItem(60, 0, false, 99), 0, &d));
        CHECK_EQ(d.cap, 1u);
        CHECK_EQ(q.size(), std::size_t(32));
        // game frames do not use the handshake's room, and flooding them never touches a handshake frame
        for (int i = 0; i < 500; ++i) {
            q.Push(TxItem(60, 0, true, 7), 0, &d);
        }
        CHECK_EQ(q.size(), std::size_t(32 + 128));
        q.PushFront(TxItem(10, 0, false, 55)); // the join request
        const auto tags = TxTags(q, 0, &d);
        CHECK_EQ(tags.size(), std::size_t(32 + 128 + 1));
        CHECK_EQ(tags[0], 55);
        for (int i = 0; i < 32; ++i) {
            CHECK_EQ(tags[1 + i], static_cast<u8>(100 + i));
        }
    }
    // a reference model: random pushes and takes agree with a plain re-implementation of the rules
    {
        struct Ref {
            std::deque<TxQueue::Item> items;
            u32 cap = 0, stale = 0;
            u32 frames() const {
                u32 n = 0;
                for (const auto& i : items) n += i.expendable;
                return n;
            }
            u32 bytes() const {
                u32 n = 0;
                for (const auto& i : items) n += i.expendable ? static_cast<u32>(i.bytes.size()) : 0;
                return n;
            }
            u32 protecteds() const {
                u32 n = 0;
                for (const auto& i : items) n += !i.expendable;
                return n;
            }
            void Drop(u64 now) {
                for (auto it = items.begin(); it != items.end();) {
                    if (it->expendable && now > it->queued_ms && now - it->queued_ms > 1000) {
                        it = items.erase(it);
                        ++stale;
                    } else {
                        ++it;
                    }
                }
            }
            void Push(TxQueue::Item item, u64 now) {
                if (item.expendable) {
                    if (item.bytes.size() > 32768) {
                        ++cap;
                        return;
                    }
                    Drop(now);
                    while (frames() >= 128 || bytes() + item.bytes.size() > 32768) {
                        for (auto it = items.begin(); it != items.end(); ++it) {
                            if (it->expendable) {
                                items.erase(it);
                                break;
                            }
                        }
                        ++cap;
                    }
                } else if (protecteds() >= 32) {
                    ++cap;
                    return;
                }
                items.push_back(std::move(item));
            }
        };
        u64 x = 0x9E3779B97F4A7C15ull;
        auto rnd = [&x](u32 n) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            return static_cast<u32>(x % n);
        };
        TxQueue q;
        Ref ref;
        TxQueue::Drops d;
        u64 now = 0;
        for (int step = 0; step < 20000; ++step) {
            now += rnd(40);
            if (rnd(60) == 0) {
                now += 800 + rnd(600); // a stall
            }
            if (rnd(25) == 0) {
                TxQueue::Drops td;
                const auto tags = TxTags(q, now, &td);
                d.stale += td.stale;
                ref.Drop(now);
                std::vector<u8> expected;
                for (const auto& i : ref.items) expected.push_back(i.bytes[0]);
                ref.items.clear();
                CHECK(tags == expected);
                CHECK(q.empty());
                continue;
            }
            const bool expendable = rnd(8) != 0;
            const u32 size = expendable ? 20 + rnd(rnd(30) == 0 ? 3000 : 600) : 30 + rnd(120);
            const u8 tag = static_cast<u8>(step);
            TxQueue::Drops pd;
            q.Push(TxItem(size, now, expendable, tag), now, &pd);
            d.cap += pd.cap;
            d.stale += pd.stale;
            ref.Push(TxItem(size, now, expendable, tag), now);
            CHECK_EQ(q.size(), ref.items.size());
        }
        CHECK_EQ(d.cap, ref.cap);
        CHECK_EQ(d.stale, ref.stale);
    }
}

static void TestEncodeAllocations() {
    std::printf("frames: one allocation each\n");
    for (u32 size : {0u, 1u, 40u, 280u, 1478u}) {
        std::vector<u8> data(size, 0x5A);
        const std::vector<u8> payload = GenerateDataPayload(data.data(), data.size(), 1, 2, 3, 4);
        CHECK_EQ(payload.capacity(), payload.size()); // reserved once, nothing grew
        CHECK_EQ(payload.size(), kLlcHeaderSize + kSecureDataHeaderSize + size);
        WifiPacket p;
        p.type = WifiPacket::Type::Data;
        p.channel = 11;
        p.data = payload;
        const std::vector<u8> message = room::EncodeWifiPacket(p);
        CHECK_EQ(message.capacity(), message.size());
        CHECK_EQ(message.size(), 19 + payload.size());
        WifiPacket back;
        CHECK(room::DecodeWifiPacket(message.data(), message.size(), &back));
        CHECK(back.data == payload);
    }
}

static void TestContendedMutex() {
    std::printf("engine lock: contention is counted, an uncontended lock is not\n");
    u32 contended = 0, longest = 0;
    Ticks64 waited = 0, net_waited = 0;
    platform::ContendedMutex m;
    m.Watch({&contended, &waited, &longest, &net_waited});
    for (int i = 0; i < 1000; ++i) {
        platform::LockGuard lock(m);
    }
    CHECK_EQ(contended, 0u);
    CHECK_EQ(waited, 0u);
    std::atomic<bool> holding{false};
    std::thread holder([&] {
        platform::LockGuard lock(m);
        holding = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    });
    while (!holding.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
        platform::LockGuard lock(m); // has to wait for the holder
    }
    holder.join();
    CHECK_EQ(contended, 1u);
    CHECK(azl_ticks_to_ms(waited) >= 20 && azl_ticks_to_ms(waited) < 2000);
    CHECK_EQ(static_cast<u64>(longest), static_cast<u64>(waited));
    CHECK_EQ(net_waited, 0u); // (a game thread: not the network thread's share)
    // The network thread's wait is its own: counted apart, and noted for the loop that takes the waits off its CPU time.
    {
        const u64 noted_before = platform::NetWaitTicks();
        std::atomic<bool> holding2{false};
        std::thread holder2([&] {
            platform::LockGuard lock(m);
            holding2 = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        });
        while (!holding2.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const u64 total_before = waited;
        {
            const platform::NetThreadMark net_thread; // (this thread is the network thread for a moment)
            CHECK(platform::IsNetThread());
            platform::LockGuard lock(m); // has to wait for the holder
        }
        holder2.join();
        CHECK(!platform::IsNetThread());
        CHECK_EQ(contended, 2u);
        const u64 mine = waited - total_before;
        CHECK(azl_ticks_to_ms(mine) >= 20 && azl_ticks_to_ms(mine) < 2000);
        CHECK_EQ(static_cast<u64>(net_waited), mine);                    // the whole wait is the network thread's
        CHECK_EQ(platform::NetWaitTicks() - noted_before, mine);         // ... and was noted for the loop
        // a wait of a thread that is not marked is not noted, whatever it waited for
        platform::NoteNetWait(12345);
        CHECK_EQ(platform::NetWaitTicks() - noted_before, mine);
    }
    // an unwatched mutex works too
    platform::ContendedMutex plain;
    { platform::LockGuard lock(plain); }
    // the engine's own mutex feeds g_stats: a kernel call the engine makes under its lock holds it for a while (Bind
    // creates the event of the bind under the lock), and a status read from another thread has to wait
    struct SlowCreateKernel : ShortWaitKernel {
        u32 CreateEvent() override {
            if (armed.exchange(false)) {
                inside = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
            }
            return ShortWaitKernel::CreateEvent();
        }
        std::atomic<bool> armed{false}, inside{false};
    } kernel;
    std::memset(&g_stats, 0, sizeof(g_stats));
    MockRoom room;
    Console a(room, 1, &kernel);
    u32 event = 0;
    CHECK_EQ(a.engine->Initialize(MakeNode(0x1001, "Lock", 0), kTestSharedMemory, kTestVersion, &event), 0u);
    CHECK_EQ(g_stats.lock_contended, 0u); // (nobody else was there)
    const u32 holds_before = g_stats.lock_holds;
    kernel.armed = true;
    std::thread binding([&] {
        u32 bind_event = 0;
        a.engine->Bind(2, 0x5F4, 1, kBroadcastNetworkNodeId, &bind_event);
    });
    for (int i = 0; i < 5000 && !kernel.inside.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(kernel.inside.load());
    a.engine->GetConnectionStatus(); // waits for the binding thread
    binding.join();
    CHECK_EQ(g_stats.lock_contended, 1u);
    CHECK_EQ(g_stats.lock_holds - holds_before, 2u); // (the Bind and the status read: every entry point goes through the guard)
    CHECK(azl_ticks_to_ms(g_stats.lock_wait_ticks) >= 20 && azl_ticks_to_ms(g_stats.lock_wait_ticks) < 2000);
    CHECK_EQ(static_cast<u64>(g_stats.lock_wait_max), static_cast<u64>(g_stats.lock_wait_ticks));
    CHECK_EQ(g_stats.lock_wait_net_ticks, 0u); // (the status read came from this thread, which is not the network thread)
    // with the log on the guard measures how long the lock was held: the Bind's 60 ms, by a game thread
    g_debug_level = 1;
    const u32 hold_before = g_stats.lock_hold_max;
    kernel.armed = true;
    std::thread binding2([&] {
        u32 bind_event = 0;
        a.engine->Bind(3, 0x5F4, 2, kBroadcastNetworkNodeId, &bind_event);
    });
    binding2.join();
    g_debug_level = 0;
    CHECK(g_stats.lock_hold_max > hold_before && azl_ticks_to_ms(g_stats.lock_hold_max) >= 50 && azl_ticks_to_ms(g_stats.lock_hold_max) < 2000);
    CHECK_EQ(g_stats.lock_hold_max_net, 0u);
    CHECK(g_stats.lock_hold_ticks >= g_stats.lock_hold_max);
}

// The signals of the engine are given after its lock is released: the game thread a signal wakes (PullPacket after the
// bind's event, a status read after the status event) never finds the lock taken by the thread that signalled it. The
// data in the ring is there before the signal.
static void TestSignalsAfterUnlock() {
    std::printf("engine lock: events are signalled after the lock is released (the data path, the status event), SendTo hands over without it\n");
    g_rules.Reset();
    struct SlowSignalKernel : ShortWaitKernel {
        void SignalEvent(u32 h) override {
            if (h == watched.load() && armed.exchange(false)) {
                inside = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                inside = false;
            }
            ShortWaitKernel::SignalEvent(h);
        }
        std::atomic<u32> watched{0};
        std::atomic<bool> armed{false}, inside{false};
    };
    const auto wait_inside = [](SlowSignalKernel& k) {
        for (int i = 0; i < 5000 && !k.inside.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return k.inside.load();
    };
    MockRoom room;
    NetworkInfo ni = MakeNetworkInfo(4, 4);
    AzaharHost azhost(room, 1, ni, MakeNode(0x1111, "AzHost", 0));
    Bench alice(room, 2);
    SlowSignalKernel kernel;
    Console bob(room, 3, &kernel);
    u32 bob_status = 0;
    CHECK_EQ(alice.Init(), 0u);
    CHECK_EQ(bob.engine->Initialize(MakeNode(0x1003, "Bob", 0), kTestSharedMemory, kTestVersion, &bob_status), 0u);
    azhost.SendBeacon();
    room.WaitIdle();
    NetworkInfo net;
    CHECK(alice.ScanFirst(&net));
    CHECK_EQ(alice.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1), 0u);
    room.WaitIdle();
    CHECK_EQ(bob.engine->ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1), 0u);
    room.WaitIdle();
    u32 bob_bind = 0;
    CHECK_EQ(bob.engine->Bind(2, 0x2E30, 1, 0xFFFF, &bob_bind), 0u);
    azhost.TakeInbox();
    std::memset(&g_stats, 0, sizeof(g_stats));

    // 1. the data path: the room's thread stores alice's frame in bob's ring and signals the bind's event with the lock
    // free; bob's PullPacket, made while the signal is still in progress, finds the frame at once
    const u8 msg[] = "unicast";
    kernel.watched = bob_bind;
    kernel.armed = true;
    CHECK_EQ(alice.e().SendTo(3, 1, sizeof(msg), 0, msg, sizeof(msg)), 0u);
    CHECK(wait_inside(kernel));
    const u64 t0 = platform::NowMs();
    u16 src = 0;
    u32 size = 0;
    const std::vector<u8> got = PullAll(bob, 2, &src, &size);
    const u64 pull_ms = platform::NowMs() - t0;
    CHECK(pull_ms < 30); // (not the 60 ms of the signal: the lock was not held through it)
    CHECK_EQ(size, u32(sizeof(msg)));
    CHECK_EQ(src, 2);
    CHECK_EQ(g_stats.lock_contended, 0u);
    room.WaitIdle();
    CHECK(kernel.Signals(bob_bind) >= 1);

    // 2. the status event: DisconnectNetwork signals it after unlocking, and a status read meanwhile does not wait
    kernel.watched = bob_status;
    kernel.armed = true;
    std::thread leaving([&] { bob.engine->DisconnectNetwork(); });
    CHECK(wait_inside(kernel));
    const u64 t1 = platform::NowMs();
    bob.engine->GetConnectionStatus();
    CHECK(platform::NowMs() - t1 < 30);
    leaving.join();
    CHECK_EQ(g_stats.lock_contended, 0u);
    room.WaitIdle();

    // 3. SendTo builds the frame and hands it to the room without the engine's lock: a slow hand-over (the test's
    // sniff hook sleeps inside it) does not make a status read of another thread wait
    std::atomic<bool> armed{true}, inside{false};
    alice.console.sniff = [&](const WifiPacket&) {
        if (armed.exchange(false)) {
            inside = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            inside = false;
        }
    };
    std::thread sending([&] { alice.e().SendTo(1, 1, sizeof(msg), 0, msg, sizeof(msg)); });
    for (int i = 0; i < 5000 && !inside.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(inside.load());
    const u64 t2 = platform::NowMs();
    alice.Status();
    CHECK(platform::NowMs() - t2 < 30);
    sending.join();
    CHECK_EQ(g_stats.lock_contended, 0u);
    alice.console.sniff = nullptr;
    room.WaitIdle();
}

// Tick (the network thread, every round) takes the lock only when a beacon is due, a member's absence is being timed, or
// every 8th round for the bookkeeping: an idle engine ticked 64 times takes it 8 times.
static void TestTickWithoutLock() {
    std::printf("engine tick: the lock is taken only when there is something to do with it\n");
    g_rules.Reset();
    std::memset(&g_stats, 0, sizeof(g_stats));
    MockRoom room;
    Bench idle(room, 1);
    CHECK_EQ(idle.Init(), 0u);
    const u32 before = g_stats.lock_holds;
    const u64 base = platform::NowMs();
    for (int i = 0; i < 64; ++i) {
        idle.e().Tick(base + static_cast<u64>(i) * 3);
    }
    CHECK_EQ(g_stats.lock_holds - before, 0u);
    // a host: the rounds in which a beacon is due take it too, and the beacons keep their schedule
    MockRoom room2;
    Bench host(room2, 2);
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(host.BeginHosting(4), 0u);
    std::atomic<u32> beacons{0};
    host.console.sniff = [&beacons](const WifiPacket& p) {
        if (p.type == WifiPacket::Type::Beacon) {
            ++beacons;
        }
    };
    const u64 base2 = platform::NowMs();
    const u32 before2 = g_stats.lock_holds;
    for (int i = 0; i < 100; ++i) {
        host.e().Tick(base2 + static_cast<u64>(i) * 3); // 300 ms: two beacons (102.4 ms apart) ...
    }
    room2.WaitIdle();
    CHECK(beacons.load() >= 2 && beacons.load() <= 3);
    // ... each one a lock, plus what the room's delivery of the beacons costs the host's own engine (OnWifiPacket)
    CHECK(g_stats.lock_holds - before2 <= 3 * beacons.load() + 2);
    host.console.sniff = nullptr;
}

// How long the network thread may sit in the socket: none when a frame is queued, 1 ms for 10 ms after a frame, else 3.
static void TestServiceTimeoutRule() {
    std::printf("net loop: the wait in the socket follows the game's frames (0 / 1 / 3 ms)\n");
    CHECK_EQ(kServiceTimeoutMs, 3u);
    CHECK_EQ(kQuickPollMs, 1u);
    CHECK_EQ(kQuickPollWindowMs, 10u);
    CHECK_EQ(ServiceTimeoutFor(true, 1000, 0), 0u);
    CHECK_EQ(ServiceTimeoutFor(true, 1000, 999), 0u);
    CHECK_EQ(ServiceTimeoutFor(false, 1000, 995), kQuickPollMs);
    CHECK_EQ(ServiceTimeoutFor(false, 1009, 1000), kQuickPollMs);
    CHECK_EQ(ServiceTimeoutFor(false, 1010, 1000), kServiceTimeoutMs);
    CHECK_EQ(ServiceTimeoutFor(false, 5, 0xFFFFFFFCu), kQuickPollMs); // the truncated clock wrapped: 9 ms after
    CHECK_EQ(ServiceTimeoutFor(false, 100, 0), kServiceTimeoutMs);
    CHECK_EQ(ServiceTimeoutFor(false, 0, 0), kQuickPollMs); // (the first ms of a session: harmless)

    // how a frame goes out at each level: the handshake always reliable and single; beacons and broadcast data unreliable from
    // level 1; unicast game data reliable at level 1, unreliable and doubled at level 2
    // (level, beacon, game data, broadcast, the destination numbers its frames)
    const auto d = [](int level, bool beacon, bool game, bool broadcast, bool peer = true) {
        return DeliveryFor(level, beacon, game, broadcast, peer);
    };
    for (int level = 0; level <= 2; ++level) {
        CHECK(d(level, false, false, false).reliable && !d(level, false, false, false).duplicate); // the handshake
        CHECK(d(level, false, false, true).reliable && !d(level, false, false, true).duplicate);   // (a broadcast one too)
    }
    CHECK(d(0, true, false, true).reliable && d(0, false, true, true).reliable && d(0, false, true, false).reliable);
    CHECK(!d(1, true, false, true).reliable && !d(1, true, false, true).duplicate);
    CHECK(!d(1, false, true, true).reliable && !d(1, false, true, true).duplicate);
    CHECK(d(1, false, true, false).reliable && !d(1, false, true, false).duplicate);
    CHECK(d(1, false, true, false, false).reliable && !d(1, false, true, false, false).duplicate); // (level 1 does not care who the peer is)
    CHECK(!d(2, true, false, true).reliable && !d(2, true, false, true).duplicate);
    CHECK(!d(2, false, true, true).reliable && !d(2, false, true, true).duplicate);
    CHECK(!d(2, false, true, false).reliable && d(2, false, true, false).duplicate);      // a peer with this plugin: twice, no retries
    CHECK(d(2, false, true, false, false).reliable && !d(2, false, true, false, false).duplicate); // an Azahar, an older plugin: once, reliable
    RoomClient levels;
    levels.SetFastData(true);
    CHECK(levels.fast_level() == 1 && levels.fast_data());
    levels.SetFastData(2);
    CHECK(levels.fast_level() == 2 && levels.fast_data());
    levels.SetFastData(7);
    CHECK_EQ(levels.fast_level(), 2);
    levels.SetFastData(false);
    CHECK(levels.fast_level() == 0 && !levels.fast_data());
}

static void TestStatsSinks() {
    std::printf("statistics sinks: soc calls, datagram gaps, drain per service call\n");
    std::memset(&g_stats, 0, sizeof(g_stats));
    // The soc calls are the network thread's: a call counted from any other thread (the PC tests run a room server in the
    // same process, with the same ENet layer) is not in the statistics and is not a wait of the loop
    const u64 noted_before = platform::NetWaitTicks();
    StatsSocCall(kSocSend, 777);
    CHECK_EQ(g_stats.soc_calls[kSocSend], 0u);
    CHECK_EQ(g_stats.soc_ticks[kSocSend], 0u);
    CHECK_EQ(platform::NetWaitTicks(), noted_before);
    const platform::NetThreadMark net_thread;
    // 8 recv calls, every one timed (the sum is exact: 0.1.15 timed one in four and scaled it up)
    for (int i = 0; i < 8; ++i) {
        StatsSocCall(kSocRecvData, i == 3 || i == 7 ? 1000 : 500);
    }
    CHECK_EQ(g_stats.soc_calls[kSocRecvData], 8u);
    CHECK_EQ(g_stats.soc_ticks[kSocRecvData], 2u * 1000 + 6u * 500);
    CHECK_EQ(g_stats.soc_max_ticks[kSocRecvData], 1000u);
    CHECK_EQ(g_stats.soc_over20[kSocRecvData], 0u);
    // ... and each one is a wait of the loop, noted for it
    CHECK_EQ(platform::NetWaitTicks() - noted_before, 2u * 1000 + 6u * 500);
    // a call longer than 2^32 ticks (16 s) neither wraps the sum nor the maximum
    StatsSocCall(kSocRecvEmpty, (1ull << 32) + 5);
    CHECK_EQ(g_stats.soc_ticks[kSocRecvEmpty], (1ull << 32) + 5);
    CHECK_EQ(g_stats.soc_max_ticks[kSocRecvEmpty], 0xFFFFFFFFu);
    CHECK_EQ(g_stats.soc_over100[kSocRecvEmpty], 1u);
    g_stats.soc_ticks[kSocRecvEmpty] = 0;
    g_stats.soc_calls[kSocRecvEmpty] = 0;
    g_stats.soc_max_ticks[kSocRecvEmpty] = 0;
    g_stats.soc_over20[kSocRecvEmpty] = 0;
    g_stats.soc_over100[kSocRecvEmpty] = 0;
    StatsSocCall(kSocPoll, 21 * AZL_TICKS_PER_MS);
    StatsSocCall(kSocPoll, 20 * AZL_TICKS_PER_MS);
    StatsSocCall(kSocPoll, 100 * AZL_TICKS_PER_MS);
    StatsSocCall(kSocPoll, 101 * AZL_TICKS_PER_MS);
    CHECK_EQ(g_stats.soc_calls[kSocPoll], 4u);
    CHECK_EQ(g_stats.soc_over20[kSocPoll], 3u);   // 21, 100, 101 ms (20 ms is not more than 20 ms)
    CHECK_EQ(g_stats.soc_over100[kSocPoll], 1u);  // 101 ms
    CHECK_EQ(g_stats.soc_max_ticks[kSocPoll], 101 * AZL_TICKS_PER_MS);
    CHECK_EQ(g_stats.soc_calls[kSocSend], 0u);

    // the gaps between datagrams: only the long ones are bucketed
    std::memset(&g_stats, 0, sizeof(g_stats));
    const u64 ms = AZL_TICKS_PER_MS;
    for (u64 gap : {u64(0), 3 * ms, 49 * ms, 50 * ms, 99 * ms, 100 * ms, 149 * ms, 150 * ms, 249 * ms, 250 * ms, 499 * ms,
                    500 * ms, 1900 * ms}) {
        StatsRoomDatagram(100, gap);
    }
    StatsRoomDatagram(100, 2000 * ms);  // a quiet room: counted as a datagram, not as a hold
    StatsRoomDatagram(100, 90000 * ms);
    CHECK_EQ(g_stats.rx_datagrams, 15u);
    CHECK_EQ(g_stats.gap_hist[0], 2u); // 50, 99
    CHECK_EQ(g_stats.gap_hist[1], 2u); // 100, 149
    CHECK_EQ(g_stats.gap_hist[2], 2u); // 150, 249
    CHECK_EQ(g_stats.gap_hist[3], 2u); // 250, 499
    CHECK_EQ(g_stats.gap_hist[4], 2u); // 500, 1900
    // what one service call drained
    CHECK_EQ(g_stats.drain_dgrams_cur, 15u);
    CHECK_EQ(g_stats.drain_bytes_cur, 1500u);
    CHECK_EQ(StatsServiceCall(5 * AZL_TICKS_PER_MS), 15u);
    CHECK_EQ(g_stats.drain_dgrams_max, 15u);
    CHECK_EQ(g_stats.drain_bytes_max, 1500u);
    CHECK_EQ(g_stats.service_max_ticks, 5 * AZL_TICKS_PER_MS);
    StatsRoomDatagram(50, 0);
    CHECK_EQ(StatsServiceCall(2 * AZL_TICKS_PER_MS), 1u);
    CHECK_EQ(g_stats.drain_dgrams_max, 15u);                          // the maximum stays
    CHECK_EQ(g_stats.service_max_ticks, 5 * AZL_TICKS_PER_MS);
    CHECK_EQ(g_stats.drain_dgrams_cur, 0u);
    std::memset(&g_stats, 0, sizeof(g_stats));
}

// A deterministic stream of frames as the room could relay them: mostly secure data for every sort of
// node pair, with the management bit, other channels, short and cut payloads, other frame types and
// messages whose length field lies.
namespace {

struct PrefilterRng {
    u64 x = 0x2545F4914F6CDD1Dull;
    u32 operator()(u32 n) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return static_cast<u32>((x >> 11) % n);
    }
};

std::vector<std::vector<u8>> PrefilterFrames(u16 own_id, const Mac& own_mac, const Mac& host_mac, int count) {
    PrefilterRng rnd;
    std::vector<std::vector<u8>> frames;
    const Mac macs[] = {kBroadcastMac, own_mac, host_mac, Mac{0, 0x1F, 0x32, 0, 0, 0x77}, Mac{}};
    const u16 nodes[] = {own_id, 0xFFFF, 1, 2, 3, 4, 0, 7, 0x7FFF};
    for (int i = 0; i < count; ++i) {
        WifiPacket p;
        // (no Deauthentication: one from the host would end the session the others are played into)
        static const WifiPacket::Type kOther[] = {WifiPacket::Type::Beacon, WifiPacket::Type::Authentication,
                                                  WifiPacket::Type::AssociationResponse, WifiPacket::Type::NodeMap};
        p.type = rnd(10) == 0 ? kOther[rnd(4)] : WifiPacket::Type::Data;
        p.channel = static_cast<u8>(rnd(3) == 0 ? rnd(256) : 11);
        p.transmitter_address = macs[rnd(5)];
        p.destination_address = macs[rnd(5)];
        const u32 payload = rnd(4) == 0 ? 1 + rnd(1470) : (rnd(3) == 0 ? 1 : 40 + rnd(240)); // (a pull of 0 bytes is an empty one, of more than 1478 refused)
        std::vector<u8> data(payload);
        for (u8& b : data) {
            b = static_cast<u8>(rnd(256));
        }
        const u16 dest = nodes[rnd(9)];
        const u16 src = rnd(8) == 0 ? static_cast<u16>(rnd(0x10000)) : nodes[rnd(9)];
        std::vector<u8> frame = GenerateDataPayload(data.data(), data.size(), static_cast<u8>(rnd(3) == 0 ? rnd(256) : 1),
                                                    dest, src, static_cast<u16>(rnd(0x10000)));
        if (rnd(12) == 0) {
            frame[kLlcHeaderSize + 6] = 1; // management
        }
        if (rnd(15) == 0 && dest != own_id && dest != 0xFFFF) { // (a frame for the game with a lie in it would block its bind)
            StoreBE16(frame.data() + kLlcHeaderSize + 0, static_cast<u16>(rnd(5) == 0 ? 0xFFFF : rnd(40))); // lying size
        }
        if (rnd(15) == 0) {
            StoreBE16(frame.data() + 6, rnd(2) ? 0x888E : static_cast<u16>(rnd(0x10000))); // another ether type
        }
        if (rnd(10) == 0) {
            frame.resize(rnd(40)); // cut
        }
        p.data = std::move(frame);
        std::vector<u8> message = room::EncodeWifiPacket(p);
        if (rnd(14) == 0) {
            message.resize(rnd(static_cast<u32>(message.size()) + 1)); // the datagram lost its tail
        }
        if (rnd(14) == 0 && message.size() >= 19) {
            StoreBE32(message.data() + 15, rnd(4000)); // the length field lies
        }
        frames.push_back(std::move(message));
    }
    return frames;
}

struct PrefilterOutcome {
    Stats stats;
    std::vector<std::string> debug;
    std::vector<std::string> delivered; // what the game would pull, per bind, in order
    int discarded = 0;
};

enum class Role { Uninitialised, Idle, Host, Client, Spectator, Connecting, Stopped };

// Plays `frames` into an engine in `role`, the way RoomClient::Run does: a prefilter that says "handled"
// ends it, anything else is decoded and given to OnWifiPacket.
PrefilterOutcome RunPrefilter(Role role, bool use_prefilter, int count) {
    PrefilterOutcome out;
    MockRoom room;
    Bench host(room, 1), subject(room, 2);
    UdsEngine* engine = nullptr;
    u16 own_id = 0;
    Mac own_mac = subject.console.mac;
    std::thread connecting; // the game's Connect that stays open in the role Connecting
    switch (role) {
    case Role::Connecting: {
        subject.Init();
        subject.kernel.cap_ns = 60000000000LL; // (the join stays open for the whole run: Shutdown ends it)
        NetworkInfo nobody = MakeNetworkInfo(4, 0);
        nobody.host_mac_address = Mac{0x00, 0x1F, 0x32, 0x00, 0x00, 0x55};
        nobody.channel = 11;
        connecting = std::thread([&subject, nobody]() mutable {
            subject.e().ConnectToNetwork(reinterpret_cast<u8*>(&nobody), sizeof(nobody), 1);
        });
        for (int i = 0; i < 5000 && subject.e().status() != NetworkStatus::Connecting; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        engine = &subject.e();
        break;
    }
    case Role::Stopped:
        subject.Init();
        subject.e().StopService(kServiceSleep);
        engine = &subject.e();
        break;
    case Role::Uninitialised:
        engine = &subject.e();
        break;
    case Role::Idle:
        subject.Init();
        engine = &subject.e();
        break;
    case Role::Host:
        subject.Init();
        subject.BeginHosting(4);
        engine = &subject.e();
        own_id = 1;
        break;
    case Role::Client:
    case Role::Spectator:
        host.Init();
        host.BeginHosting(4);
        subject.Init();
        JoinBench(room, subject, host, role == Role::Client ? 1 : 2);
        engine = &subject.e();
        own_id = subject.Status().network_node_id;
        break;
    }
    room.WaitIdle();
    u32 e1 = 0, e2 = 0, e3 = 0;
    if (role != Role::Uninitialised) {
        engine->Bind(2, 0x2E30, 1, 0xFFFF, &e1);
        engine->Bind(3, 0x2E30, 243, 0xFFFF, &e2);
        engine->Bind(4, 0x2E30, 11, 2, &e3);
    }
    room.WaitIdle();
    auto frames = PrefilterFrames(own_id, own_mac, host.console.mac, count);
    if (role == Role::Connecting) {
        // The join stays open: a random frame that happens to read as the host's EAPoL answer would complete it,
        // and the Connect thread waking up in the middle of the run would add timing-dependent counters.
        frames.erase(std::remove_if(frames.begin(), frames.end(),
                                    [](const std::vector<u8>& m) {
                                        return m.size() >= 19 + 8 && LoadBE16(m.data() + 19 + 6) == 0x888E;
                                    }),
                     frames.end());
    }
    g_debug_level = 1;
    DebugRecord rec;
    u8 buf[1700];
    while (DebugPop(&rec, buf, sizeof(buf))) {
    }
    std::memset(&g_stats, 0, sizeof(g_stats));
    for (const auto& message : frames) {
        if (use_prefilter && engine->TryDiscardEncoded(message.data(), message.size())) {
            ++out.discarded;
        } else {
            WifiPacket p;
            if (room::DecodeWifiPacket(message.data(), message.size(), &p)) {
                engine->OnWifiPacket(p);
            }
        }
        while (DebugPop(&rec, buf, sizeof(buf))) {
            std::string line;
            DebugFormat(rec, buf, rec.ms, &line); // (the time reads 0)
            out.debug.push_back(line);
        }
    }
    g_debug_level = 0;
    out.stats = g_stats;
    // the engine's status block and what the binds hold
    const ConnectionStatus st = subject.Status();
    out.delivered.push_back(Hex(reinterpret_cast<const u8*>(&st), sizeof(st)));
    for (u32 bind : {2u, 3u, 4u}) {
        for (int n = 0; n < 700; ++n) {
            u16 src = 0;
            u32 size = 0;
            std::vector<u8> data;
            if (PullInto(*engine, bind, &data, &size, &src) != 0) {
                break; // (a session the random frames ended answers "wrong state")
            }
            if (size == 0) {
                break;
            }
            out.delivered.push_back(std::to_string(bind) + ":" + std::to_string(src) + ":" + Hex(data.data(), size));
        }
    }
    if (connecting.joinable()) {
        engine->Shutdown(); // ends the join that is still open
        connecting.join();
    }
    return out;
}

} // namespace

static void TestPrefilterEquivalence() {
    std::printf("prefilter: dropping a frame before it is decoded is the same as dropping it afterwards\n");
    const struct {
        Role role;
        const char* name;
    } kRoles[] = {{Role::Uninitialised, "not initialised"}, {Role::Idle, "idle"},   {Role::Host, "host"},
                  {Role::Client, "client"},                 {Role::Spectator, "spectator"},
                  {Role::Connecting, "connecting"},         {Role::Stopped, "service stopped"}};
    for (const auto& r : kRoles) {
        PrefilterOutcome full = RunPrefilter(r.role, false, 6000);
        PrefilterOutcome peek = RunPrefilter(r.role, true, 6000);
        // (the lock counters are the one difference the prefilter is for: a frame it discards never takes the lock)
        full.stats.lock_holds = peek.stats.lock_holds = 0;
        full.stats.lock_hold_max = peek.stats.lock_hold_max = 0;
        full.stats.lock_hold_max_net = peek.stats.lock_hold_max_net = 0;
        full.stats.lock_hold_ticks = peek.stats.lock_hold_ticks = 0;
        std::printf("  %-16s %d of 6000 frames dropped before the decoder, %lu delivered\n", r.name, peek.discarded,
                    static_cast<unsigned long>(full.stats.data_delivered));
        if (std::memcmp(&full.stats, &peek.stats, sizeof(Stats)) != 0) {
            const u32* a = reinterpret_cast<const u32*>(&full.stats);
            const u32* b = reinterpret_cast<const u32*>(&peek.stats);
            for (std::size_t i = 0; i < sizeof(Stats) / sizeof(u32); ++i) {
                if (a[i] != b[i]) {
                    std::printf("  counter word %zu differs: %u / %u\n", i, a[i], b[i]);
                }
            }
        }
        CHECK(std::memcmp(&full.stats, &peek.stats, sizeof(Stats)) == 0);
        CHECK(full.debug == peek.debug);
        CHECK(full.delivered == peek.delivered);
        CHECK_EQ(full.discarded, 0);
        if (r.role == Role::Client) {
            CHECK(peek.discarded > 1500); // not-for-me, echo and the rest: a large share is gone early
            CHECK(full.stats.drop_not_for_me > 0 && full.stats.drop_echo > 0);
        }
        if (r.role == Role::Spectator) {
            CHECK(peek.discarded > 500);
        }
        if (r.role == Role::Uninitialised || r.role == Role::Connecting || r.role == Role::Stopped) {
            CHECK(peek.discarded > 1000); // nothing is delivered before a session is up
            CHECK_EQ(full.stats.data_delivered, 0u);
        }
        if (r.role == Role::Host) {
            CHECK_EQ(peek.discarded, 0); // a host routes: the full path decides, always
        }
        if (r.role == Role::Client) {
            CHECK(full.stats.data_delivered > 100 && full.delivered.size() > 50); // the delivered ones still arrive
        }
    }
    // the frames themselves, one by one: a management frame, one for a host's router and a cut one are
    // never decided early
    {
        g_rules.Reset(); // (the random streams above provoke every rule on purpose)
        MockRoom room;
        Bench host(room, 1), alice(room, 2);
        host.Init();
        host.BeginHosting(4);
        alice.Init();
        CHECK_EQ(JoinBench(room, alice, host), 0u);
        const u16 me = alice.Status().network_node_id;
        const u8 data[20] = {};
        auto message = [&](u16 dest, u16 src, bool management) {
            WifiPacket p;
            p.type = WifiPacket::Type::Data;
            p.channel = 11;
            p.transmitter_address = host.console.mac;
            p.destination_address = kBroadcastMac;
            p.data = GenerateDataPayload(data, sizeof(data), 1, dest, src, 0);
            if (management) {
                p.data[kLlcHeaderSize + 6] = 1;
            }
            return room::EncodeWifiPacket(p);
        };
        std::memset(&g_stats, 0, sizeof(g_stats));
        auto m = message(me, 1, false);
        CHECK(!alice.e().TryDiscardEncoded(m.data(), m.size()));            // for us: the game gets it
        m = message(0xFFFF, 1, false);
        CHECK(!alice.e().TryDiscardEncoded(m.data(), m.size()));            // for everybody
        m = message(me + 1, 1, true);
        CHECK(!alice.e().TryDiscardEncoded(m.data(), m.size()));            // management: never early
        m = message(me + 1, 1, false);
        CHECK(alice.e().TryDiscardEncoded(m.data(), m.size()));             // for another node
        CHECK_EQ(g_stats.drop_not_for_me, 1u);
        m = message(me, me, false);
        CHECK(alice.e().TryDiscardEncoded(m.data(), m.size()));             // our own echo
        CHECK_EQ(g_stats.drop_echo, 1u);
        m.resize(m.size() - 3);
        CHECK(!alice.e().TryDiscardEncoded(m.data(), m.size()));            // its payload is shorter than it says
        CHECK(!alice.e().TryDiscardEncoded(m.data(), 10));
        CHECK(!alice.e().TryDiscardEncoded(nullptr, 0));
        // a beacon, an authentication ...: other frame types are not looked at
        WifiPacket b;
        b.type = WifiPacket::Type::Beacon;
        b.channel = 11;
        b.data = std::vector<u8>(100, 1);
        auto bm = room::EncodeWifiPacket(b);
        CHECK(!alice.e().TryDiscardEncoded(bm.data(), bm.size()));
        std::memset(&g_stats, 0, sizeof(g_stats));
        // a host does not drop frames for other nodes early (it may have to route them)
        CHECK_EQ(host.Status().status, 6u);
        auto hm = message(3, 2, false);
        CHECK(!host.e().TryDiscardEncoded(hm.data(), hm.size()));
        CHECK_EQ(g_stats.drop_not_for_me, 0u);
        CHECK_RULES();
    }
}

// What the engine puts in the debug capture for the frames it drops (spec C4): the frame of another node and our own echo are
// header-only records - the prefilter that decides early and the full path that decides late write the same line - and the
// frames for this node keep their whole payload.
static std::vector<std::string> CaptureTexts() {
    std::vector<std::string> texts;
    DebugRecord rec;
    u8 buf[1700];
    while (DebugPop(&rec, buf, sizeof(buf))) {
        std::string line;
        DebugFormat(rec, buf, rec.ms, &line);
        texts.push_back(line.substr(line.find(' ', 6) + 1)); // (without the time)
    }
    return texts;
}

static void TestCaptureOfDroppedFrames() {
    std::printf("capture: the frames dropped for being another node's or our own echo are header-only, early or late\n");
    g_rules.Reset();
    MockRoom room;
    Bench host(room, 1), alice(room, 2);
    host.Init();
    host.BeginHosting(4);
    alice.Init();
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    const u16 me = alice.Status().network_node_id;
    CHECK_EQ(me, 2);
    u32 bind_event = 0;
    CHECK_EQ(alice.e().Bind(2, 0x5F4, 1, kBroadcastNetworkNodeId, &bind_event), 0u);
    CHECK(DebugInit());
    g_debug_level = 1;
    DebugReset();
    u8 data[20];
    for (int i = 0; i < 20; ++i) {
        data[i] = static_cast<u8>(0x10 + i);
    }
    const auto packet = [&](u16 dest, u16 src) {
        WifiPacket p;
        p.type = WifiPacket::Type::Data;
        p.channel = 11;
        p.transmitter_address = host.console.mac;
        p.destination_address = kBroadcastMac;
        p.data = GenerateDataPayload(data, sizeof(data), 1, dest, src, 0);
        return p;
    };
    // the full path: a frame for another node, the echo of our own, a frame for us
    alice.e().OnWifiPacket(packet(me + 1, 1));
    alice.e().OnWifiPacket(packet(me, me));
    alice.e().OnWifiPacket(packet(me, 1));
    const std::vector<std::string> late = CaptureTexts();
    CHECK_EQ(late.size(), std::size_t(3));
    if (late.size() == 3) {
        CHECK_EQ(late[0], std::string("FRAME other 1>3 ch1 20B: \n"));
        CHECK_EQ(late[1], std::string("FRAME echo  2>2 ch1 20B: \n"));
        CHECK_EQ(late[2], std::string("FRAME rx    1>2 ch1 20B: 101112131415161718191a1b1c1d1e1f20212223\n"));
    }
    // the prefilter (the network thread asks it first, on the encoded message): the same two header-only lines
    const auto encoded = [&](u16 dest, u16 src) { return room::EncodeWifiPacket(packet(dest, src)); };
    auto m = encoded(me + 1, 1);
    CHECK(alice.e().TryDiscardEncoded(m.data(), m.size()));
    m = encoded(me, me);
    CHECK(alice.e().TryDiscardEncoded(m.data(), m.size()));
    m = encoded(me, 1);
    CHECK(!alice.e().TryDiscardEncoded(m.data(), m.size())); // (for us: the full path writes the whole record)
    const std::vector<std::string> early = CaptureTexts();
    CHECK_EQ(early.size(), std::size_t(2));
    if (early.size() == 2 && late.size() == 3) {
        CHECK_EQ(early[0], late[0]);
        CHECK_EQ(early[1], late[1]);
    }
    // a cut payload: the size the header says is clipped to the bytes that came, in both paths
    {
        WifiPacket p = packet(me + 1, 1);
        p.data.resize(p.data.size() - 8);
        alice.e().OnWifiPacket(p);
        const std::vector<u8> enc = room::EncodeWifiPacket(p);
        const bool early_drop = alice.e().TryDiscardEncoded(enc.data(), enc.size());
        const std::vector<std::string> cut = CaptureTexts();
        CHECK(early_drop || cut.size() == 1);
        for (const std::string& line : cut) {
            CHECK_EQ(line, std::string("FRAME other 1>3 ch1 12B: \n"));
        }
    }
    // a host: its own frame given back by the room is an echo
    {
        WifiPacket echo = packet(me, 1);
        echo.transmitter_address = alice.console.mac;
        host.e().OnWifiPacket(echo);
        const std::vector<std::string> host_lines = CaptureTexts();
        CHECK_EQ(host_lines.size(), std::size_t(1));
        if (host_lines.size() == 1) {
            CHECK_EQ(host_lines[0], std::string("FRAME echo  1>2 ch1 20B: \n"));
        }
    }
    g_debug_level = 0;
    DebugReset();
    CHECK_RULES();
}

// Which node has which MAC (spec C7): a line when the host admits a client or loses one and when a client gets the host's node
// map, with the names the games gave their nodes.
static void TestNodeMapLines() {
    std::printf("log: the node-to-MAC list when a client joins or leaves, on the host and on the clients\n");
    g_rules.Reset();
    std::vector<std::string> lines;
    std::mutex lines_mutex;
    SetLogSink([&](const char* line) {
        std::lock_guard<std::mutex> l(lines_mutex);
        lines.push_back(line);
    });
    g_debug_level = 1;
    {
        MockRoom room;
        NetworkInfo ni = MakeNetworkInfo(4, 6);
        Console host(room, 1), alice(room, 2), bob(room, 3);
        u32 ev;
        CHECK_EQ(host.engine->Initialize(MakeNode(0x1111, "Host", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        CHECK_EQ(alice.engine->Initialize(MakeNode(0x2222, "Alice", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        CHECK_EQ(bob.engine->Initialize(MakeNode(0x3333, "Bob", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        CHECK_EQ(host.engine->BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni)), 0u);
        // (the Logoff frames that reach alice, to say the last one again below)
        std::vector<WifiPacket> logoffs_seen;
        std::mutex logoffs_mutex;
        alice.tap = [&](const WifiPacket& p) {
            u16 magic = 0;
            if (p.type == WifiPacket::Type::Data && GetEapolFrameType(p.data, &magic) && magic == kEapolLogoffMagic) {
                std::lock_guard<std::mutex> l(logoffs_mutex);
                logoffs_seen.push_back(p);
            }
        };
        auto join = [&](Console& client) {
            host.engine->Tick(platform::NowMs() + 100000);
            room.WaitIdle();
            std::vector<u8> scan(0x4000);
            CHECK_EQ(client.engine->RecvBeaconBroadcastData(scan.data(), static_cast<u32>(scan.size()), kBroadcastMac), 0u);
            NetworkInfo scanned;
            CHECK(FirstScannedNetwork(scan, &scanned));
            CHECK_EQ(client.engine->ConnectToNetwork(reinterpret_cast<u8*>(&scanned), sizeof(scanned), 1), 0u);
            room.WaitIdle();
        };
        join(alice);
        join(bob);
        // the same list is not said twice: the host's last Logoff frame once more (a repeated broadcast) changes nothing, says nothing
        {
            WifiPacket again;
            {
                std::lock_guard<std::mutex> l(logoffs_mutex);
                CHECK(!logoffs_seen.empty());
                if (!logoffs_seen.empty()) {
                    again = logoffs_seen.back();
                }
            }
            const auto said_lists = [&] {
                std::lock_guard<std::mutex> l(lines_mutex);
                std::size_t n = 0;
                for (const std::string& line : lines) {
                    n += line.rfind("uds: nodes (", 0) == 0 ? 1 : 0;
                }
                return n;
            };
            const std::size_t before_replay = said_lists();
            CHECK(before_replay >= 4);
            alice.engine->OnWifiPacket(again);
            alice.engine->OnWifiPacket(again);
            CHECK_EQ(said_lists(), before_replay);
        }
        CHECK_EQ(bob.engine->DisconnectNetwork(), 0u);
        room.WaitIdle();
        CHECK_EQ(alice.engine->Shutdown(), 0u);
        CHECK_EQ(host.engine->Shutdown(), 0u);
        CHECK_EQ(bob.engine->Shutdown(), 0u);
        CHECK_RULES();
    }
    g_debug_level = 0;
    DebugReset(); // (the role lines of the engines above are in the capture: the next test counts its own)
    SetLogSink([](const char*) {});
    auto has = [&](const std::string& needle) {
        for (const std::string& line : lines) {
            if (line == needle) {
                return true;
            }
        }
        return false;
    };
    const std::string host_mac = "00:1F:32:00:00:01";
    const std::string alice_mac = "00:1F:32:00:00:02";
    const std::string bob_mac = "00:1F:32:00:00:03";
    const std::string two = "node 1=" + host_mac + " 'Host', node 2=" + alice_mac + " 'Alice'";
    const std::string three = two + ", node 3=" + bob_mac + " 'Bob'";
    // the host, after each client it admitted
    CHECK(has("uds: nodes (a client joined): " + two));
    CHECK(has("uds: nodes (a client joined): " + three));
    // ... and when one sent its goodbye
    CHECK(has("uds: node 3 sent a Deauthentication: removed"));
    CHECK(has("uds: nodes (a node left): " + two));
    // a client, when the host's Logoff frame arrives (the node map came just before it: MACs in, names now)
    CHECK(has("uds: nodes (the host's Logoff frame): " + two));
    CHECK(has("uds: nodes (the host's Logoff frame): " + three));
    // the same list is not said twice in a row by one engine
    int joined = 0;
    for (const std::string& line : lines) {
        joined += line == "uds: nodes (a client joined): " + two ? 1 : 0;
    }
    CHECK_EQ(joined, 1);
}

static void TestEngineEventCounters() {
    std::printf("engine counters: twin candidates, deauthentications, removed nodes, connect outcomes\n");
    g_rules.Reset();
    MockRoom room;
    // twins: bob's frame for alice reaches her direct and again through the Azahar host
    {
        NetworkInfo ni = MakeNetworkInfo(4, 6);
        AzaharHost azhost(room, 1, ni, MakeNode(0x1111, "AzHost", 0));
        Console alice(room, 2), bob(room, 3);
        u32 ev, bind_ev;
        CHECK_EQ(alice.engine->Initialize(MakeNode(0x2222, "Alice", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        CHECK_EQ(bob.engine->Initialize(MakeNode(0x3333, "Bob", 0), kTestSharedMemory, kTestVersion, &ev), 0u);
        CHECK_EQ(alice.engine->Bind(2, 0x5F4, 1, kBroadcastNetworkNodeId, &bind_ev), 0u);
        azhost.SendBeacon();
        room.WaitIdle();
        std::vector<u8> scan(0x4000);
        CHECK_EQ(alice.engine->RecvBeaconBroadcastData(scan.data(), static_cast<u32>(scan.size()), kBroadcastMac), 0u);
        NetworkInfo scanned;
        CHECK(FirstScannedNetwork(scan, &scanned));
        std::memset(&g_stats, 0, sizeof(g_stats));
        CHECK_EQ(alice.engine->ConnectToNetwork(reinterpret_cast<u8*>(&scanned), sizeof(scanned), 1), 0u);
        room.WaitIdle();
        CHECK_EQ(bob.engine->ConnectToNetwork(reinterpret_cast<u8*>(&scanned), sizeof(scanned), 1), 0u);
        room.WaitIdle();
        CHECK_EQ(g_stats.connect_result[0], 2u);
        CHECK_EQ(g_stats.connect_result[1] + g_stats.connect_result[2], 0u);
        std::memset(&g_stats, 0, sizeof(g_stats));
        const u8 msg[] = "to alice";
        for (int i = 0; i < 5; ++i) {
            CHECK_EQ(bob.engine->SendTo(2, 1, sizeof(msg), 2, msg, sizeof(msg)), 0u); // the broadcast flag
        }
        room.WaitIdle();
        CHECK_EQ(g_stats.data_delivered, 5u);    // the 5 direct ones; the 5 relayed by the host are their twins
        CHECK_EQ(g_stats.fwd_candidates, 5u);
        CHECK_EQ(g_stats.drop_fwd_dup, 5u);
        CHECK_EQ(g_stats.fwd_unmatched, 0u);
        // a frame the host sent itself is no twin
        std::memset(&g_stats, 0, sizeof(g_stats));
        CHECK(azhost.SendData(2, 1, 2, std::vector<u8>(msg, msg + sizeof(msg))));
        room.WaitIdle();
        CHECK_EQ(g_stats.data_delivered, 1u);
        CHECK_EQ(g_stats.fwd_candidates, 0u);
        // alice leaves: the Azahar host sees one Deauthentication (its model counts, the engine does not)
        CHECK_EQ(alice.engine->DisconnectNetwork(), 0u);
        CHECK_EQ(bob.engine->DisconnectNetwork(), 0u);
        room.WaitIdle();
    }
    // a 3DS host: removals by cause, deauthentications by role, connect outcomes
    {
        Bench host(room, 11), alice(room, 12), bob(room, 13), carol(room, 14);
        for (Bench* b : {&host, &alice, &bob, &carol}) {
            CHECK_EQ(b->Init(), 0u);
        }
        CHECK_EQ(host.BeginHosting(4), 0u);
        std::memset(&g_stats, 0, sizeof(g_stats));
        CHECK_EQ(JoinBench(room, alice, host), 0u);
        CHECK_EQ(JoinBench(room, bob, host), 0u);
        CHECK_EQ(JoinBench(room, carol, host), 0u);
        CHECK_EQ(g_stats.connect_result[0], 3u);
        // alice leaves politely: the host takes her out on her Deauthentication
        CHECK_EQ(alice.e().DisconnectNetwork(), 0u);
        room.WaitIdle();
        CHECK_EQ(g_stats.deauth_rx[0], 1u);
        CHECK_EQ(g_stats.node_removed[0], 1u);
        // the host throws bob out
        CHECK_EQ(host.e().EjectClient(3), 0u);
        room.WaitIdle();
        CHECK_EQ(g_stats.node_removed[2], 1u);
        // carol's association replaces itself when she authenticates again: it is no leave
        std::memset(&g_stats, 0, sizeof(g_stats));
        WifiPacket auth;
        auth.type = WifiPacket::Type::Authentication;
        auth.channel = 11;
        auth.transmitter_address = carol.console.mac;
        auth.destination_address = host.console.mac;
        auth.data = GenerateAuthenticationFrame(AuthSeq::Seq1);
        host.e().OnWifiPacket(auth);
        room.WaitIdle();
        CHECK_EQ(g_stats.node_removed[1], 1u);
        CHECK_EQ(g_stats.node_removed[0] + g_stats.node_removed[2], 0u);
        // the host ends the network: the clients still in it are discarded (a client counts a deauth)
        Bench dave(room, 15);
        CHECK_EQ(dave.Init(), 0u);
        CHECK_EQ(JoinBench(room, dave, host), 0u);
        std::memset(&g_stats, 0, sizeof(g_stats));
        CHECK_EQ(host.e().DestroyNetwork(), 0u);
        room.WaitIdle();
        CHECK(g_stats.deauth_rx[1] >= 1);
        // a join the host never answers ends after the wait: counted as a silent host
        std::memset(&g_stats, 0, sizeof(g_stats));
        NetworkInfo gone = MakeNetworkInfo(4, 0);
        gone.host_mac_address = Mac{0x00, 0x1F, 0x32, 0x00, 0x00, 0x55};
        gone.channel = 11;
        CHECK_EQ(alice.e().ConnectToNetwork(reinterpret_cast<u8*>(&gone), sizeof(gone), 1), kConnectTimeout);
        CHECK_EQ(g_stats.connect_result[1], 1u);
        CHECK_RULES();
    }
}

static void TestOwnNodeInfoSeed() {
    std::printf("friend code seed: a peer announcing ours is reported once per session\n");
    LogOn log_on;
    std::vector<std::string> lines;
    SetLogSink([&lines](const char* line) { lines.push_back(line); });
    {
        g_rules.Reset();
        MockRoom room;
        Bench host(room, 21), alice(room, 22);
        // the same seed on both consoles (Azahar players may all announce one)
        CHECK_EQ(host.e().Initialize(MakeNode(0xABCD, "Host", 0), kTestSharedMemory, kTestVersion, &host.status_event), 0u);
        CHECK_EQ(alice.e().Initialize(MakeNode(0xABCD, "Alice", 0), kTestSharedMemory, kTestVersion, &alice.status_event), 0u);
        CHECK_EQ(host.BeginHosting(4), 0u);
        CHECK_EQ(JoinBench(room, alice, host), 0u);
        room.WaitIdle();
    }
    SetLogSink([](const char*) {});
    int warnings = 0;
    for (const std::string& l : lines) {
        warnings += l.find("announces the same friend code seed") != std::string::npos;
    }
    CHECK_EQ(warnings, 2); // (each console says it once, when it first sees the other's seed)
}

// ---------------------------------------------------------------------------------------------
// The room link and the game's session (spec F07, F15). A lost link ends the session the way a console's
// radio failing does; a player who leaves the room takes its node along; neither puts a frame on the wire.
// ---------------------------------------------------------------------------------------------
namespace {

constexpr u32 kConnectionFailed = 0xC92113FB;

std::vector<Mac> Members(std::initializer_list<const Bench*> benches) {
    std::vector<Mac> macs;
    for (const Bench* b : benches) {
        macs.push_back(b->console.mac);
    }
    return macs;
}

/// What a console sent that is not a beacon (a host's beacons go on all the time).
std::size_t NonBeacons(Bench& b) {
    return b.sent.Total() - static_cast<std::size_t>(b.sent.Count(WifiPacket::Type::Beacon));
}

/// A clock reading `ms` from now, for an engine's Tick (the debounce of the room lists runs on it).
u64 In(u64 ms) {
    return platform::NowMs() + ms;
}

} // namespace

static void TestTransportDown() {
    std::printf("room link lost: a client drops to idle (reason 5), a join fails, a host loses its players\n");
    g_rules.Reset();
    std::memset(&g_stats, 0, sizeof(g_stats));
    MockRoom room;
    Bench host(room, 1), alice(room, 2), bob(room, 3), idle(room, 4), spec(room, 5), carol(room, 6);
    const NetworkStatusChangeReason lost = NetworkStatusChangeReason::ConnectionLost;

    // Nothing to lose: never initialised, idle, stopped.
    CHECK(!idle.e().OnTransportDown(lost));
    CHECK_EQ(idle.Init(), 0u);
    CHECK(!idle.e().OnTransportDown(lost));
    CHECK_EQ(idle.Signals(idle.status_event), 0u);
    idle.e().StopService(kServiceSleep);
    const u32 stopped_events = idle.Signals(idle.status_event);
    CHECK(!idle.e().OnTransportDown(lost));
    CHECK_EQ(idle.Signals(idle.status_event), stopped_events);
    CHECK_EQ(g_stats.transport_down[0] + g_stats.transport_down[1] + g_stats.transport_down[2], 0u);

    for (Bench* b : {&host, &alice, &bob, &spec, &carol}) {
        CHECK_EQ(b->Init(), 0u);
    }
    CHECK_EQ(host.BeginHosting(4), 0u);
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    CHECK_EQ(JoinBench(room, bob, host), 0u);
    CHECK_EQ(JoinBench(room, spec, host, 2), 0u);

    // A client (spec U1): idle, reason 5, the status event and every bind event once, no Deauthentication, the
    // node id and the bind stay, the data commands answer "wrong state"; a second call changes nothing.
    u32 bind_event = 0;
    CHECK_EQ(alice.e().Bind(2, 0x2E30, 0xF3, 0xFFFF, &bind_event), 0u);
    const u8 hello[] = "hello";
    CHECK_EQ(host.e().SendTo(2, 0xF3, sizeof(hello), 0, hello, sizeof(hello)), 0u);
    room.WaitIdle();
    alice.Status(); // (reads the changed nodes of the join)
    alice.sent.Clear();
    const u32 events = alice.Signals(alice.status_event), binds = alice.Signals(bind_event);
    CHECK(alice.e().OnTransportDown(lost));
    ConnectionStatus st = alice.Status();
    CHECK_EQ(st.status, 3u);
    CHECK_EQ(st.status_change_reason, 5u); // CONNECTION_LOST
    CHECK_EQ(st.network_node_id, 2);
    CHECK_EQ(st.node_bitmask, 0);
    CHECK_EQ(st.total_nodes, 0);
    CHECK_EQ(alice.Signals(alice.status_event), events + 1);
    CHECK_EQ(alice.Signals(bind_event), binds + 1);
    CHECK_EQ(alice.sent.Total(), std::size_t(0));
    u16 src = 0;
    u32 size = 0;
    std::vector<u8> out;
    CHECK_EQ(PullInto(alice.e(), 2, &out, &size, &src), kW);
    CHECK_EQ(alice.e().SendTo(1, 1, 1, 0, hello, 1), kW);
    CHECK(!alice.e().OnTransportDown(lost));
    CHECK_EQ(alice.Signals(alice.status_event), events + 1);
    CHECK_EQ(alice.Signals(bind_event), binds + 1);
    const u32 not_connected = g_stats.drop_not_connected;
    CHECK_EQ(host.e().SendTo(0xFFFF, 0xF3, 1, 0, hello, 1), 0u);
    room.WaitIdle();
    CHECK(g_stats.drop_not_connected > not_connected); // (a frame for an idle console is nobody's business)
    CHECK_EQ(g_stats.transport_down[0], 1u);
    // Still initialised: the game scans and joins again at once. The host still has the old node (nobody told
    // it): the new Authentication replaces it. The bind it made before survived; the frame that was waiting in it
    // did not (it belongs to the session before).
    CHECK(alice.e().initialized());
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    st = alice.Status();
    CHECK_EQ(st.status, 9u);
    CHECK_EQ(st.network_node_id, 2);
    CHECK_EQ(PullInto(alice.e(), 2, &out, &size, &src), 0u);
    CHECK_EQ(size, 0u);
    CHECK_EQ(host.e().SendTo(2, 0xF3, sizeof(hello), 0, hello, sizeof(hello)), 0u);
    room.WaitIdle();
    CHECK_EQ(PullInto(alice.e(), 2, &out, &size, &src), 0u);
    CHECK_EQ(size, u32(sizeof(hello)));
    // a reason the caller names is the reason the game reads
    CHECK(alice.e().OnTransportDown(NetworkStatusChangeReason::RequestFromSystem));
    CHECK_EQ(alice.Status().status_change_reason, 3u);

    // A spectator drops the same way.
    CHECK(spec.e().OnTransportDown(lost));
    st = spec.Status();
    CHECK_EQ(st.status, 3u);
    CHECK_EQ(st.status_change_reason, 5u);

    // A join in progress fails (0xC92113FB, at once, not after the 5 s of a silent host); nothing is sent for it.
    {
        NetworkInfo gone = MakeNetworkInfo(4, 0);
        gone.host_mac_address = Mac{0x00, 0x1F, 0x32, 0x00, 0x00, 0x55};
        gone.channel = 11;
        u32 result = 0xFFFFFFFF;
        std::thread joining([&] { result = carol.e().ConnectToNetwork(reinterpret_cast<u8*>(&gone), sizeof(gone), 1); });
        CHECK(WaitForStatus(carol, NetworkStatus::Connecting));
        carol.sent.Clear();
        const auto began = std::chrono::steady_clock::now();
        CHECK(carol.e().OnTransportDown(lost));
        joining.join();
        const long long took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
        CHECK_EQ(result, kConnectionFailed);
        CHECK(took < 200); // (the test kernel waits 300 ms for a host that does not answer)
        st = carol.Status();
        CHECK_EQ(st.status, 3u);
        CHECK_EQ(st.status_change_reason, 0u); // BEFORE_COMMUNICATION: it never got that far
        CHECK_EQ(carol.sent.Total(), std::size_t(0));
        CHECK_EQ(g_stats.transport_down[1], 1u);
    }

    // A host (spec U2): stays a host and keeps beaconing, loses every player (bitmask and count back to itself,
    // the changed bits set), tells the game once, sends nothing; whoever comes back can join.
    host.Status();
    host.sent.Clear();
    const u32 host_events = host.Signals(host.status_event);
    CHECK(host.e().OnTransportDown(lost));
    st = host.Status();
    CHECK_EQ(st.status, 6u);
    CHECK_EQ(st.node_bitmask, 1);
    CHECK_EQ(st.total_nodes, 1);
    CHECK_EQ(st.changed_nodes, 0x0006);
    CHECK_EQ(host.Signals(host.status_event), host_events + 1);
    NodeInfo node;
    CHECK_EQ(host.e().GetNodeInformation(2, &node), kNodeNotFound);
    CHECK_EQ(host.e().GetNodeInformation(3, &node), kNodeNotFound);
    CHECK_EQ(NonBeacons(host), std::size_t(0));
    const int beacons = host.sent.Count(WifiPacket::Type::Beacon);
    SendBeacon(host);
    CHECK(host.sent.Count(WifiPacket::Type::Beacon) > beacons);
    CHECK(!host.e().OnTransportDown(lost)); // nobody left to lose
    CHECK_EQ(host.Signals(host.status_event), host_events + 1);
    CHECK_EQ(g_stats.transport_down[2], 1u);
    CHECK(g_stats.node_removed[4] >= 2);
    CHECK_EQ(alice.e().DisconnectNetwork(), 0u); // (alice and bob did not go anywhere in this test)
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    CHECK_EQ(host.Status().total_nodes, 2);
    CHECK_RULES();
}

static void TestRoomPresence() {
    std::printf("room members: a player out of the member list for 3 s has left; nothing goes on the wire\n");
    g_rules.Reset();
    std::memset(&g_stats, 0, sizeof(g_stats));
    MockRoom room;
    Bench host(room, 1), alice(room, 2), bob(room, 3), carol(room, 4);
    for (Bench* b : {&host, &alice, &bob, &carol}) {
        CHECK_EQ(b->Init(), 0u);
    }
    CHECK_EQ(host.BeginHosting(4), 0u);
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    CHECK_EQ(JoinBench(room, bob, host), 0u);
    host.Status();

    // No list yet, or only the first one of a join: it says who is there and concludes nothing, however it
    // looks (bob is missing from it: he was never seen, so he cannot have left).
    host.e().Tick(In(10000));
    CHECK_EQ(host.Status().total_nodes, 3);
    host.e().OnRoomMembers(Members({&host, &alice}));
    host.e().Tick(In(10000));
    CHECK_EQ(host.Status().total_nodes, 3);

    // A player who is gone from the list for 2 s and back is still there (the ghost of a quick rejoin).
    host.e().OnRoomMembers(Members({&host, &alice, &bob}));
    host.e().OnRoomMembers(Members({&host, &alice}));
    host.e().Tick(In(2000));
    CHECK_EQ(host.Status().total_nodes, 3);
    host.e().OnRoomMembers(Members({&host, &alice, &bob}));
    host.e().Tick(In(10000));
    CHECK_EQ(host.Status().total_nodes, 3);

    // Gone for good: his node is removed once, the game is told once, nothing is sent (spec U3).
    host.sent.Clear();
    const u32 events = host.Signals(host.status_event);
    host.e().OnRoomMembers(Members({&host, &alice}));
    host.e().Tick(In(2500));
    CHECK_EQ(host.Status().total_nodes, 3);
    host.e().Tick(In(4000));
    ConnectionStatus st = host.Status();
    CHECK_EQ(st.total_nodes, 2);
    CHECK_EQ(st.node_bitmask, 3);
    CHECK_EQ(st.changed_nodes, 0x0004);
    CHECK_EQ(host.Signals(host.status_event), events + 1);
    NodeInfo node;
    CHECK_EQ(host.e().GetNodeInformation(3, &node), kNodeNotFound);
    host.e().Tick(In(9000));
    CHECK_EQ(host.Signals(host.status_event), events + 1);
    CHECK_EQ(NonBeacons(host), std::size_t(0));
    CHECK_EQ(g_stats.node_removed[3], 1u);
    // His own MAC missing from a list means nothing (the room lists the member it is talking to).
    host.e().OnRoomMembers(Members({&alice}));
    host.e().Tick(In(9000));
    CHECK(host.e().status() == NetworkStatus::ConnectedAsHost);
    CHECK_EQ(host.Signals(host.status_event), events + 1);

    // After a rejoin of the room the first list is ignored again (spec U8): alice is not in it, she stays; once she has
    // been in a list and leaves it she goes.
    host.e().ResetRoomPresence();
    host.e().OnRoomMembers(Members({&host}));
    host.e().Tick(In(10000));
    CHECK_EQ(host.Status().total_nodes, 2);
    host.e().OnRoomMembers(Members({&host, &alice, &carol}));
    host.e().OnRoomMembers(Members({&host}));
    host.e().Tick(In(4000));
    CHECK_EQ(host.Status().total_nodes, 1); // alice went; carol was never a node
    CHECK_EQ(host.Signals(host.status_event), events + 2);
    CHECK_EQ(NonBeacons(host), std::size_t(0));

    // A client (spec U4): the departed client's node goes locally, the session stays a client's.
    CHECK_EQ(alice.e().DisconnectNetwork(), 0u);
    CHECK_EQ(bob.e().DisconnectNetwork(), 0u);
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    CHECK_EQ(JoinBench(room, bob, host), 0u);
    alice.Status();
    alice.sent.Clear();
    const u32 alice_events = alice.Signals(alice.status_event);
    alice.e().ResetRoomPresence();
    alice.e().OnRoomMembers(Members({&host, &alice, &bob}));
    alice.e().OnRoomMembers(Members({&host, &alice}));
    alice.e().Tick(In(4000));
    st = alice.Status();
    CHECK_EQ(st.status, 9u);
    CHECK_EQ(st.total_nodes, 2);
    CHECK_EQ(st.node_bitmask, 3);
    CHECK_EQ(st.changed_nodes, 0x0004);
    CHECK_EQ(alice.Signals(alice.status_event), alice_events + 1);
    CHECK_EQ(alice.sent.Total(), std::size_t(0));
    // Her own MAC missing from a list means nothing (the room lists the member it is talking to): she keeps her node.
    alice.e().ResetRoomPresence();
    alice.e().OnRoomMembers(Members({&host, &alice}));
    alice.e().OnRoomMembers(Members({&host}));
    alice.e().Tick(In(4000));
    st = alice.Status();
    CHECK_EQ(st.status, 9u);
    CHECK_EQ(st.node_bitmask, 3);
    CHECK_EQ(alice.Signals(alice.status_event), alice_events + 1);

    // The host's member leaves the room (spec U5): the network is gone, reason 5, once, silently.
    u32 bind_event = 0;
    CHECK_EQ(alice.e().Bind(2, 0x2E30, 0xF3, 0xFFFF, &bind_event), 0u);
    alice.e().ResetRoomPresence();
    alice.e().OnRoomMembers(Members({&host, &alice, &bob}));
    alice.e().OnRoomMembers(Members({&alice, &bob}));
    alice.e().Tick(In(2000));
    CHECK(alice.e().status() == NetworkStatus::ConnectedAsClient);
    alice.e().Tick(In(4000));
    st = alice.Status();
    CHECK_EQ(st.status, 3u);
    CHECK_EQ(st.status_change_reason, 5u);
    CHECK_EQ(alice.Signals(alice.status_event), alice_events + 2);
    CHECK_EQ(alice.Signals(bind_event), 1u);
    CHECK_EQ(alice.sent.Total(), std::size_t(0));
    CHECK_EQ(g_stats.presence_host_gone, 1u);

    // ... and a join that is still going on fails the same way, with 0xC92113FB.
    {
        NetworkInfo gone = MakeNetworkInfo(4, 0);
        gone.host_mac_address = Mac{0x00, 0x1F, 0x32, 0x00, 0x00, 0x55};
        gone.channel = 11;
        u32 result = 0xFFFFFFFF;
        std::thread joining([&] { result = carol.e().ConnectToNetwork(reinterpret_cast<u8*>(&gone), sizeof(gone), 1); });
        CHECK(WaitForStatus(carol, NetworkStatus::Connecting));
        carol.e().ResetRoomPresence();
        std::vector<Mac> with_host = {gone.host_mac_address, carol.console.mac};
        carol.e().OnRoomMembers(with_host);
        carol.e().OnRoomMembers(Members({&carol}));
        carol.e().Tick(In(4000));
        joining.join();
        CHECK_EQ(result, kConnectionFailed);
        CHECK_EQ(carol.Status().status, 3u);
    }
    // An engine that is not up ignores the lists altogether.
    Bench off(room, 7);
    off.e().OnRoomMembers(Members({&off, &host}));
    off.e().OnRoomMembers(Members({&off}));
    off.e().Tick(In(4000));
    CHECK_EQ(off.Signals(off.status_event), 0u);
    CHECK_RULES();
}

// A player that stays in the room but tells it that it plays nothing (Azahar closed its emulation: no goodbye on the air) is
// counted out of the game's session like one that left (Kirby Battle Royale 2026-10-28: the 3DS host waited 30 s for it).
static void TestRoomMemberStopsPlaying() {
    std::printf("room members: a player that plays nothing any more is taken out of the session after 3 s, like one that left\n");
    g_rules.Reset();
    std::memset(&g_stats, 0, sizeof(g_stats));
    MockRoom room;
    Bench host(room, 1), alice(room, 2), bob(room, 3);
    for (Bench* b : {&host, &alice, &bob}) {
        CHECK_EQ(b->Init(), 0u);
    }
    CHECK_EQ(host.BeginHosting(4), 0u);
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    CHECK_EQ(JoinBench(room, bob, host), 0u);
    host.Status();
    // What Plugin::OnRoomInfo gives the engine: the MACs of the members that play something.
    struct Seen {
        const Bench* bench;
        const char* game;
        u64 id;
    };
    const auto playing = [](std::initializer_list<Seen> seen) {
        std::vector<Mac> macs;
        for (const Seen& s : seen) {
            if (RoomMemberPlays(s.game, s.id)) {
                macs.push_back(s.bench->console.mac);
            }
        }
        return macs;
    };
    const u64 kGame = 0x00040000001C2100ull;
    host.sent.Clear();
    const u32 events = host.Signals(host.status_event);
    host.e().OnRoomMembers(playing({{&host, "Kirby Battle Royale", kGame}, {&alice, "Kirby Battle Royale", kGame}, {&bob, "Kirby Battle Royale", kGame}}));
    host.e().Tick(In(10000));
    CHECK_EQ(host.Status().total_nodes, 3);
    // Bob stops playing but stays in the room: listed with no title and no id. Within the debounce nothing happens ...
    host.e().OnRoomMembers(playing({{&host, "Kirby Battle Royale", kGame}, {&alice, "Kirby Battle Royale", kGame}, {&bob, "", 0}}));
    host.e().Tick(In(2500));
    CHECK_EQ(host.Status().total_nodes, 3);
    // ... and after it his node goes: once, the game told once, nothing on the wire.
    host.e().Tick(In(4000));
    ConnectionStatus st = host.Status();
    CHECK_EQ(st.total_nodes, 2);
    CHECK_EQ(st.node_bitmask, 3);
    CHECK_EQ(st.changed_nodes, 0x0004);
    CHECK_EQ(host.Signals(host.status_event), events + 1);
    CHECK_EQ(NonBeacons(host), std::size_t(0));
    CHECK_EQ(g_stats.node_removed[3], 1u);
    // Alice stops playing and plays again within 2 s (a game started over): she stays.
    host.e().OnRoomMembers(playing({{&host, "Kirby Battle Royale", kGame}, {&alice, "", 0}}));
    host.e().Tick(In(2000));
    host.e().OnRoomMembers(playing({{&host, "Kirby Battle Royale", kGame}, {&alice, "Kirby Battle Royale", kGame}}));
    host.e().Tick(In(10000));
    CHECK_EQ(host.Status().total_nodes, 2);
    CHECK_EQ(host.Signals(host.status_event), events + 1);
    // A member with another game's id is a member that plays: it is NOT counted out (cross-region lobbies stay as they were).
    host.e().OnRoomMembers(playing({{&host, "Kirby Battle Royale", kGame}, {&alice, "Kirby Battle Royale", 0x0004000000AAAA00ull}}));
    host.e().Tick(In(10000));
    CHECK_EQ(host.Status().total_nodes, 2);
    // ... and so is one with an id and no title, or a title and no id (a console that only knows its process name).
    host.e().OnRoomMembers(playing({{&host, "Kirby Battle Royale", kGame}, {&alice, "", kGame}}));
    host.e().Tick(In(10000));
    CHECK_EQ(host.Status().total_nodes, 2);
    host.e().OnRoomMembers(playing({{&host, "Kirby Battle Royale", kGame}, {&alice, "IbizaCit", 0}}));
    host.e().Tick(In(10000));
    CHECK_EQ(host.Status().total_nodes, 2);
    CHECK_EQ(host.Signals(host.status_event), events + 1);
    CHECK_RULES();
}

// ---------------------------------------------------------------------------------------------
// The link (0.1.16): the beacon timer, the late second Deauthentication, the frames of another lobby in the room
// ---------------------------------------------------------------------------------------------
namespace {

/// The beacon times (ms of the clock the engine is given) of a run of Tick() calls with the steps `step()` returns.
template <class Step>
std::vector<double> BeaconTimes(Bench& host, u64 base, u64 duration_ms, Step step) {
    std::vector<double> times;
    std::size_t seen = static_cast<std::size_t>(host.sent.Count(WifiPacket::Type::Beacon));
    for (u64 t = 0; t <= duration_ms; t += step()) {
        host.e().Tick(base + t);
        const std::size_t now = static_cast<std::size_t>(host.sent.Count(WifiPacket::Type::Beacon));
        for (; seen < now; ++seen) {
            times.push_back(static_cast<double>(t));
        }
    }
    return times;
}

double MeanInterval(const std::vector<double>& times) {
    return times.size() < 2 ? 0.0 : (times.back() - times.front()) / static_cast<double>(times.size() - 1);
}

} // namespace

// L5: the beacon is due every 102.4 ms of an absolute schedule, whatever the ticks do. Re-arming it from the moment it
// was sent (the old rule: next = now + 102) made 105.5 ms per beacon on hardware and 110 ms with 10 ms ticks.
static void TestBeaconSchedule() {
    std::printf("host beacons: an absolute schedule on the exact period (102.4 ms), no drift from late ticks\n");
    g_rules.Reset();
    MockRoom room;
    Bench host(room, 1);
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(host.BeginHosting(4), 0u);
    const u64 base = platform::NowMs();

    // 10 ms ticks for a minute
    std::vector<double> t10 = BeaconTimes(host, base, 60000, [] { return 10u; });
    std::printf("  10 ms ticks: %zu beacons in 60 s, mean interval %.2f ms\n", t10.size(), MeanInterval(t10));
    CHECK(MeanInterval(t10) > 102.0 && MeanInterval(t10) < 102.8);
    CHECK(t10.size() >= 580);     // 9.5 beacons a second at the least
    for (std::size_t i = 1; i < t10.size(); ++i) {
        CHECK(t10[i] - t10[i - 1] >= 90 && t10[i] - t10[i - 1] <= 120); // (never a burst, never a long hole)
    }

    // ticks of 3 to 15 ms, the way the network loop's rounds come: a minute more
    u32 lcg = 12345;
    std::vector<double> tj = BeaconTimes(host, base + 70000, 60000, [&lcg] {
        lcg = lcg * 1103515245u + 12345u;
        return 3u + ((lcg >> 16) % 13u);
    });
    std::printf("  3-15 ms ticks: %zu beacons in 60 s, mean interval %.2f ms\n", tj.size(), MeanInterval(tj));
    CHECK(MeanInterval(tj) > 101.4 && MeanInterval(tj) < 103.4);
    CHECK(tj.size() >= 570);

    // a stall of the network thread (a second without a tick): ONE beacon when it comes back, not a burst, and the
    // schedule goes on from there
    const u64 resume = base + 140000;
    host.e().Tick(resume - 1000);
    const int before = host.sent.Count(WifiPacket::Type::Beacon);
    host.e().Tick(resume);
    CHECK_EQ(host.sent.Count(WifiPacket::Type::Beacon), before + 1);
    host.e().Tick(resume + 10);
    CHECK_EQ(host.sent.Count(WifiPacket::Type::Beacon), before + 1);
    std::vector<double> after = BeaconTimes(host, resume + 20, 1000, [] { return 10u; });
    CHECK(after.size() >= 8 && after.size() <= 10);
    CHECK(after.front() >= 60 && after.front() <= 100); // the next one is a period after the resume beacon
    CHECK_RULES();
}

namespace {

/// A data frame of a transmitter that belongs to no lobby of ours, as the room would carry it: node ids as given.
void SendStranger(RawClient& from, const Mac& to, u16 dest, u16 src, u8 channel, u8 seq) {
    const std::vector<u8> body = {0xF0, seq, static_cast<u8>(src), static_cast<u8>(dest), 'f', 'o', 'r', 'e', 'i', 'g', 'n'};
    from.Send(WifiPacket::Type::Data, to, GenerateDataPayload(body.data(), body.size(), channel, dest, src, 0));
}

/// A member of the room that sends exactly what it is given, unchecked by the rule checker: the host of another lobby, say.
class Voice {
public:
    Voice(MockRoom& room, u8 mac_last) : mac{0x00, 0x1F, 0x32, 0x00, 0x00, mac_last}, room_(room) {
        index_ = room.AddMember(mac, [](const WifiPacket&) {});
    }
    ~Voice() {
        room_.RemoveMember(index_);
    }
    void Send(WifiPacket::Type type, const Mac& to, std::vector<u8> data) {
        WifiPacket p;
        p.type = type;
        p.channel = 11;
        p.transmitter_address = mac;
        p.destination_address = to;
        p.data = std::move(data);
        room_.Send(index_, p);
    }
    Mac mac;

private:
    MockRoom& room_;
    int index_ = -1;
};

void SendStranger(Voice& from, const Mac& to, u16 dest, u16 src, u8 channel, u8 seq) {
    const std::vector<u8> body = {0xF0, seq, static_cast<u8>(src), static_cast<u8>(dest), 'f', 'o', 'r', 'e', 'i', 'g', 'n'};
    from.Send(WifiPacket::Type::Data, to, GenerateDataPayload(body.data(), body.size(), channel, dest, src, 0));
}

/// What a bind holds: how many frames, how many of them are strangers' (first byte 0xF0).
struct Drained {
    int frames = 0;
    int foreign = 0;
    std::map<int, int> by_source;
};
Drained Drain(UdsEngine& e, u32 bind_id) {
    Drained d;
    for (;;) {
        std::vector<u8> out;
        u32 size = 0;
        u16 src = 0;
        CHECK_EQ(PullInto(e, bind_id, &out, &size, &src), 0u);
        if (size == 0) {
            return d;
        }
        ++d.frames;
        d.foreign += out[0] == 0xF0 ? 1 : 0;
        ++d.by_source[src];
    }
}

} // namespace

// L6: Azahar's guests send their goodbye twice, 59 ms to 2.04 s apart, and a host removes a node by MAC: the second copy of
// the first goodbye must not end the association a node made in between.
static void TestLateSecondDeauth() {
    std::printf("host: the late second Deauthentication of a node that joined again does not end its new association\n");
    g_rules.Reset();
    std::memset(&g_stats, 0, sizeof(g_stats));
    MockRoom room;
    Bench host(room, 1);
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(host.BeginHosting(4), 0u);
    host.Status();
    RawClient a(room, 2), b(room, 3);
    const Mac H = host.console.mac;
    auto join = [&](RawClient& c) {
        c.Auth(H);
        room.WaitIdle();
        c.Start(H);
        room.WaitIdle();
    };
    auto nodes = [&] { return host.Status().total_nodes; };

    // a plain join and leave: the Deauthentication takes effect at once; its second copy finds nothing to end
    join(a);
    CHECK_EQ(nodes(), 2);
    a.Deauth(H);
    room.WaitIdle();
    CHECK_EQ(nodes(), 1);
    a.Deauth(H);
    room.WaitIdle();
    CHECK_EQ(nodes(), 1);
    CHECK_EQ(g_stats.deauth_late, 0u);

    // it joins again at once: the late second copy of the first goodbye is swallowed, once ...
    join(a);
    CHECK_EQ(nodes(), 2);
    a.Deauth(H);
    room.WaitIdle();
    CHECK_EQ(nodes(), 2);
    CHECK_EQ(host.Status().node_bitmask, 3);
    CHECK_EQ(g_stats.deauth_late, 1u);
    // ... and the next Deauthentication is the node really leaving
    a.Deauth(H);
    room.WaitIdle();
    CHECK_EQ(nodes(), 1);
    CHECK_EQ(g_stats.deauth_late, 1u);

    // the late copy arrives between the Authentication and the Start of the new association: the Start still admits it
    a.Auth(H);
    room.WaitIdle();
    a.Deauth(H);
    room.WaitIdle();
    a.Start(H);
    room.WaitIdle();
    CHECK_EQ(nodes(), 2);
    CHECK_EQ(g_stats.deauth_late, 2u);
    a.Deauth(H);
    a.Deauth(H);
    room.WaitIdle();
    CHECK_EQ(nodes(), 1);

    // a node that never left lately has no copy on its way: its Deauthentication is acted on at once
    join(b);
    CHECK_EQ(nodes(), 2);
    b.Deauth(H);
    room.WaitIdle();
    CHECK_EQ(nodes(), 1);
    CHECK_EQ(g_stats.deauth_late, 2u);

    // the guard lasts 3 s from the new Authentication: a Deauthentication after that is the node leaving; and a node
    // that left more than 3 s ago has no guard at all (two clients that have not left before: nothing is on record yet)
    RawClient c(room, 4), d(room, 5);
    join(c);
    join(d);
    CHECK_EQ(nodes(), 3);
    c.Deauth(H);
    d.Deauth(H);
    room.WaitIdle();
    CHECK_EQ(nodes(), 1);
    join(d);                   // back at once: guarded
    CHECK_EQ(nodes(), 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
    join(c);                   // its goodbye was 3.1 s ago: no guard
    CHECK_EQ(nodes(), 3);
    d.Deauth(H);               // 3.1 s after its Authentication: the guard is over, this is the node leaving
    c.Deauth(H);               // no guard
    room.WaitIdle();
    CHECK_EQ(nodes(), 1);
    CHECK_EQ(g_stats.deauth_late, 2u);
    CHECK_RULES();
}

// L4, host: a second lobby of the same game in the room has the same node ids (host 1, then 2, 3, 4): its frames look like
// ours. 0.1.15 delivered them (501 frames overflowed the ring of a 3DS host and fired the "frames lost" warning); the
// transmitter is what tells them apart, and a host takes a frame only from a client it admitted.
static void TestForeignLobbyHost() {
    std::printf("foreign lobby: a host takes no frame from a transmitter that is not one of its clients\n");
    g_rules.Reset();
    MockRoom room;
    Bench host(room, 1), alice(room, 2);
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(alice.Init(), 0u);
    CHECK_EQ(host.BeginHosting(4), 0u);
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    alice.e().SetClientForeignFilterEnforced(true); // (the other lobby's Logoff must not rewrite her view of ours)
    u32 bind_event = 0;
    CHECK_EQ(host.e().Bind(2, 0x2E30, 1, kBroadcastNetworkNodeId, &bind_event), 0u);

    // the other lobby: an Azahar host and two clients, node ids 1, 2, 3 - the ones we use
    AzaharHost other(room, 0x71, MakeNetworkInfo(4, 6), MakeNode(0x7777, "OtherHost", 0));
    RawClient c2(room, 0x72), c3(room, 0x73);
    for (RawClient* c : {&c2, &c3}) {
        c->Auth(other.mac());
        room.WaitIdle();
        c->Start(other.mac());
        room.WaitIdle();
    }
    CHECK_EQ(other.total(), 3);

    // our lobby works: alice -> host
    const u8 hello[] = "alice to host";
    CHECK_EQ(alice.e().SendTo(1, 1, sizeof(hello), 2, hello, sizeof(hello)), 0u);
    room.WaitIdle();
    Drained d = Drain(host.e(), 2);
    CHECK_EQ(d.frames, 1);
    CHECK_EQ(d.foreign, 0);
    CHECK_EQ(d.by_source[2], 1);

    // the other lobby talks: its host to everybody (node 1 is OUR id), its clients to node 1 and to everybody, and one frame
    // for ANOTHER node addressed to our MAC (a router would pass it on). Alice hears them too, with her filter on.
    std::memset(&g_stats, 0, sizeof(g_stats));
    const u32 signals = host.Signals(bind_event);
    host.sent.Clear();
    const std::vector<u8> theirs = {0xF0, 1, 2, 3, 4, 5, 6, 7};
    for (int i = 0; i < 200; ++i) {
        other.SendData(kBroadcastNetworkNodeId, 1, 2, theirs);
        SendStranger(c2, kBroadcastMac, 1, 2, 1, static_cast<u8>(i));
        SendStranger(c3, kBroadcastMac, kBroadcastNetworkNodeId, 3, 1, static_cast<u8>(i));
    }
    SendStranger(c2, host.console.mac, 3, 2, 1, 0);
    room.WaitIdle();
    d = Drain(host.e(), 2);
    CHECK_EQ(d.frames, 0);                              // nothing reached the game
    CHECK_EQ(host.Signals(bind_event), signals);        // ... and the game was not woken for it
    CHECK_EQ(g_stats.data_delivered, 0u);
    CHECK_EQ(g_stats.ring_overwritten, 0u);             // (501 pushed out in the log of PROVA 2)
    // per round the host refuses three frames and alice two (the other lobby's node-2 frames are alice's own id: echo);
    // the unicast one is the host's alone
    CHECK_EQ(g_stats.drop_foreign, 3u * 200 + 2u * 200 + 1u);
    CHECK_EQ(g_stats.drop_echo, 200u);
    CHECK_EQ(g_stats.forwarded, 0u);                    // the router did not pass the stranger's frame on
    CHECK_EQ(host.sent.CountSecureData(), 0);
    CHECK_EQ(g_stats.foreign_shadow, 0u);

    // a stranger's frame that carries OUR host's node id is a stranger's frame, not our own echo
    std::memset(&g_stats, 0, sizeof(g_stats));
    SendStranger(c2, host.console.mac, 1, 1, 1, 0);
    room.WaitIdle();
    CHECK_EQ(g_stats.drop_foreign, 1u);
    CHECK_EQ(g_stats.drop_echo, 0u);

    // ours still does, to the very end
    std::memset(&g_stats, 0, sizeof(g_stats));
    CHECK_EQ(alice.e().SendTo(1, 1, sizeof(hello), 2, hello, sizeof(hello)), 0u);
    room.WaitIdle();
    d = Drain(host.e(), 2);
    CHECK_EQ(d.frames, 1);
    CHECK_EQ(d.foreign, 0);
    CHECK_EQ(g_stats.data_delivered, 1u);
    CHECK_EQ(g_stats.drop_foreign, 0u);

    // a MAC that was a node of ours and left is a stranger from then on
    RawClient r(room, 0x44);
    r.Auth(host.console.mac);
    room.WaitIdle();
    r.Start(host.console.mac);
    room.WaitIdle();
    CHECK_EQ(host.Status().total_nodes, 3);
    SendStranger(r, kBroadcastMac, 1, 3, 1, 1);         // (the first byte marks it for the count only)
    room.WaitIdle();
    CHECK_EQ(Drain(host.e(), 2).frames, 1);
    r.Deauth(host.console.mac);
    room.WaitIdle();
    CHECK_EQ(host.Status().total_nodes, 2);
    std::memset(&g_stats, 0, sizeof(g_stats));
    SendStranger(r, host.console.mac, 1, 3, 1, 2);
    room.WaitIdle();
    CHECK_EQ(g_stats.drop_foreign, 1u);
    CHECK_EQ(Drain(host.e(), 2).frames, 0);
    CHECK_RULES();
}

// L4, client: a 3DS in a lobby counts the frames of another lobby (the "shadow") and, with the filter on, drops them -
// besides the data frames also the node map and the Logoff of the other host, which would rewrite its view of the lobby.
// Nothing its own lobby sends is ever dropped.
static void TestForeignLobbyClient() {
    std::printf("foreign lobby: a client counts the frames of another lobby (shadow), drops them with the filter on, never its own lobby's\n");
    g_rules.Reset();
    MockRoom room;
    AzaharHost azhost(room, 1, MakeNetworkInfo(4, 6), MakeNode(0x1111, "AzHost", 0));
    Bench alice(room, 2), bob(room, 3);
    CHECK_EQ(alice.Init(), 0u);
    CHECK_EQ(bob.Init(), 0u);
    for (Bench* c : {&alice, &bob}) {
        azhost.SendBeacon();
        room.WaitIdle();
        NetworkInfo net;
        CHECK(c->ScanFirst(&net));
        CHECK_EQ(c->e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1), 0u);
        room.WaitIdle();
    }
    u32 alice_bind = 0, bob_bind = 0;
    CHECK_EQ(alice.e().Bind(2, 0x2E30, 1, kBroadcastNetworkNodeId, &alice_bind), 0u);
    CHECK_EQ(bob.e().Bind(2, 0x2E30, 1, kBroadcastNetworkNodeId, &bob_bind), 0u);
    Voice stranger(room, 0x61);

    // what her own lobby sends her: the host's broadcast, bob's frame (with the broadcast flag: the Azahar host re-broadcasts
    // it, so she sees the direct copy and the host's twin, which her dedupe drops), bob's frame to her MAC alone
    auto legit_round = [&](u8 seq) {
        CHECK(azhost.SendData(kBroadcastNetworkNodeId, 1, 2, {0xAA, seq, 1}));
        const u8 from_bob[] = {0xBB, seq, 3};
        const u8 from_bob_alone[] = {0xBC, seq, 3}; // (not the bytes of the other: the twin filter pairs equal ones)
        CHECK_EQ(bob.e().SendTo(2, 1, sizeof(from_bob), 2, from_bob, sizeof(from_bob)), 0u);
        CHECK_EQ(bob.e().SendTo(2, 1, sizeof(from_bob_alone), 0, from_bob_alone, sizeof(from_bob_alone)), 0u);
        room.WaitIdle();
    };

    // shadow (the default): a stranger's frames are counted and, as before, delivered
    std::memset(&g_stats, 0, sizeof(g_stats));
    for (u8 i = 0; i < 5; ++i) {
        SendStranger(stranger, kBroadcastMac, kBroadcastNetworkNodeId, 3, 1, i); // (node 3 is bob's id, and a node of her lobby)
    }
    room.WaitIdle();
    CHECK_EQ(g_stats.foreign_shadow, 5u); // (bob heard them too: for him a frame from node 3 is his own echo)
    CHECK_EQ(g_stats.drop_foreign, 0u);
    Drained d = Drain(alice.e(), 2);
    CHECK_EQ(d.foreign, 5);
    Drain(bob.e(), 2);

    // the filter on: the same frames are dropped, her own lobby's are not
    alice.e().SetClientForeignFilterEnforced(true);
    bob.e().SetClientForeignFilterEnforced(true);
    std::memset(&g_stats, 0, sizeof(g_stats));
    for (u8 round = 0; round < 10; ++round) {
        legit_round(round);
        SendStranger(stranger, kBroadcastMac, kBroadcastNetworkNodeId, 1, 1, round);     // as their host
        SendStranger(stranger, kBroadcastMac, kBroadcastNetworkNodeId, 3, 1, round);     // as bob
        SendStranger(stranger, alice.console.mac, 2, 3, 1, round);                       // to her MAC alone
        room.WaitIdle();
    }
    d = Drain(alice.e(), 2);
    CHECK_EQ(d.foreign, 0);
    CHECK_EQ(d.by_source[1], 10);           // the host's, once each
    CHECK_EQ(d.by_source[3], 20);           // bob's: the direct copy of the broadcast one and the unicast one (the twin is dropped)
    // per round: their host's broadcast is a stranger's at alice and at bob, "bob's" broadcast at alice (it is bob's own echo
    // for him), the frame to her MAC alone at alice only
    CHECK_EQ(g_stats.drop_foreign, 4u * 10);
    CHECK_EQ(g_stats.foreign_shadow, 0u);

    // the other lobby admits a client: its node map and Logoff go to the whole room. A client with the filter on does not
    // take them (they would rewrite her node list); her lobby goes on as before
    const ConnectionStatus before = alice.Status();
    const u32 events = alice.Signals(alice.status_event);
    std::memset(&g_stats, 0, sizeof(g_stats));
    std::vector<NodeInfo> theirs(4);
    theirs[0] = MakeNode(0x7777, "OtherHost", 1);
    theirs[1] = MakeNode(0x7002, "Other2", 2);
    stranger.Send(WifiPacket::Type::NodeMap, kBroadcastMac, GenerateNodeMapPacket({{Mac{0x00, 0x1F, 0x32, 0x00, 0x00, 0x72}, 2}}));
    stranger.Send(WifiPacket::Type::Data, kBroadcastMac, GenerateEapolLogoffFrame(2, theirs, 4, 2));
    room.WaitIdle();
    const ConnectionStatus after = alice.Status();
    CHECK_EQ(after.node_bitmask, before.node_bitmask);
    CHECK_EQ(after.total_nodes, before.total_nodes);
    CHECK_EQ(alice.Signals(alice.status_event), events);
    CHECK_EQ(g_stats.drop_foreign, 4u); // the node map and the Logoff, at alice and at bob
    legit_round(50);
    d = Drain(alice.e(), 2);
    CHECK_EQ(d.by_source[1], 1);
    CHECK_EQ(d.by_source[3], 2);        // bob is still in her map: the other lobby's node map did not replace it
    CHECK_EQ(g_stats.drop_foreign, 4u);

    // a filter that is switched off again lets a stranger's data through (and counts it)
    alice.e().SetClientForeignFilterEnforced(false);
    std::memset(&g_stats, 0, sizeof(g_stats));
    SendStranger(stranger, alice.console.mac, 2, 3, 1, 99);
    room.WaitIdle();
    CHECK_EQ(g_stats.foreign_shadow, 1u);
    CHECK_EQ(Drain(alice.e(), 2).foreign, 1);
    CHECK_RULES();
}

// LAT-5: a SendTo with the broadcast flag (2) and a node in mind (Heroes of Ruin, FFE) went to the broadcast MAC: through the room that is a
// datagram, a receive and an acknowledgement for every member, and in a lobby of three half of what a console receives is "not for me".
// With the node's own MAC the room delivers it to that member alone; the header is the same (the node ids are what a receiver reads), the
// frame stays unreliable (the radio does not retry a broadcast), and when the room does not have the node the broadcast is kept.
static void TestAddressedBroadcastFlag() {
    std::printf("SendTo with the broadcast flag and a node: addressed to that node's MAC when the room has it, still unreliable; broadcast when not\n");
    g_rules.Reset();
    MockRoom room;
    Bench host(room, 1), alice(room, 2), bob(room, 3);
    for (Bench* b : {&host, &alice, &bob}) {
        CHECK_EQ(b->Init(), 0u);
    }
    CHECK_EQ(host.BeginHosting(4), 0u);
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    CHECK_EQ(JoinBench(room, bob, host), 0u);
    u32 ev = 0;
    for (Bench* b : {&host, &alice, &bob}) {
        CHECK_EQ(b->e().Bind(2, 0x2E30, 1, kBroadcastNetworkNodeId, &ev), 0u);
    }
    const u16 alice_id = alice.Status().network_node_id, bob_id = bob.Status().network_node_id;
    CHECK(alice_id >= 2 && bob_id >= 2 && alice_id != bob_id);
    const u8 msg[] = "to one node";
    const auto last_data = [](Bench& b) {
        const std::vector<WifiPacket> all = b.sent.All();
        for (auto it = all.rbegin(); it != all.rend(); ++it) {
            EtherType t;
            if (it->type == WifiPacket::Type::Data && GetFrameEtherType(it->data, &t) && t == EtherType::SecureData) {
                return *it;
            }
        }
        return WifiPacket();
    };

    // 0. The room's member list is not known yet (the plugin gets it from the room's messages): the broadcast, as before.
    room.WaitIdle();
    std::memset(&g_stats, 0, sizeof(g_stats));
    CHECK_EQ(alice.e().SendTo(bob_id, 1, sizeof(msg), 2, msg, sizeof(msg)), 0u);
    room.WaitIdle();
    CHECK(last_data(alice).destination_address == kBroadcastMac);
    CHECK_EQ(g_stats.sendto_addressed, 0u);
    CHECK_EQ(Drain(bob.e(), 2).frames, 1);
    CHECK_EQ(Drain(host.e(), 2).frames, 0);

    for (Bench* b : {&host, &alice, &bob}) {
        b->e().OnRoomMembers(Members({&host, &alice, &bob}));
    }
    std::memset(&g_stats, 0, sizeof(g_stats));

    // 1. Alice -> Bob with the flag: Bob's MAC, the flag kept on the packet; Bob has the frame, the host never sees it.
    CHECK_EQ(alice.e().SendTo(bob_id, 1, sizeof(msg), 2, msg, sizeof(msg)), 0u);
    room.WaitIdle();
    WifiPacket p = last_data(alice);
    CHECK(p.destination_address == bob.console.mac);   // (before: the broadcast MAC)
    CHECK(p.broadcast_flag && !p.relayed);
    CHECK(IsBroadcastDelivery(p));                      // ... and unreliable, like a broadcast
    SecureDataHeader h;
    CHECK(ParseSecureDataHeader(p.data, &h) && h.dest_node_id == bob_id && h.src_node_id == alice_id);
    CHECK_EQ(g_stats.sendto_addressed, 1u);
    CHECK_EQ(Drain(bob.e(), 2).frames, 1);
    CHECK_EQ(Drain(host.e(), 2).frames, 0);
    CHECK_EQ(g_stats.drop_not_for_me, 0u);              // (before: the host received it and dropped it, one "not for me")

    // 2. Alice -> the host (node 1) with the flag: the host's MAC.
    CHECK_EQ(alice.e().SendTo(1, 1, sizeof(msg), 2, msg, sizeof(msg)), 0u);
    room.WaitIdle();
    CHECK(last_data(alice).destination_address == host.console.mac);
    CHECK_EQ(Drain(host.e(), 2).frames, 1);
    CHECK_EQ(Drain(bob.e(), 2).frames, 0);

    // 3. Everybody (0xFFFF) with the flag: the broadcast, as ever. Without the flag, a node: its MAC and reliable, as ever.
    CHECK_EQ(alice.e().SendTo(kBroadcastNetworkNodeId, 1, sizeof(msg), 2, msg, sizeof(msg)), 0u);
    room.WaitIdle();
    CHECK(last_data(alice).destination_address == kBroadcastMac);
    CHECK_EQ(Drain(host.e(), 2).frames, 1);
    CHECK_EQ(Drain(bob.e(), 2).frames, 1);
    CHECK_EQ(alice.e().SendTo(bob_id, 1, sizeof(msg), 0, msg, sizeof(msg)), 0u);
    room.WaitIdle();
    p = last_data(alice);
    CHECK(p.destination_address == bob.console.mac && !p.broadcast_flag && !IsBroadcastDelivery(p));
    CHECK_EQ(Drain(bob.e(), 2).frames, 1);
    CHECK_EQ(g_stats.sendto_addressed, 2u);

    // 4. Bob has left the room (the list says so; its node is still in the network for a while): the broadcast, no frame for a MAC the
    //    room would answer with an error.
    for (Bench* b : {&host, &alice}) {
        b->e().OnRoomMembers(Members({&host, &alice}));
    }
    const u32 addressed = g_stats.sendto_addressed;
    CHECK_EQ(alice.e().SendTo(bob_id, 1, sizeof(msg), 2, msg, sizeof(msg)), 0u);
    room.WaitIdle();
    CHECK(last_data(alice).destination_address == kBroadcastMac);
    CHECK_EQ(g_stats.sendto_addressed, addressed);

    // 5. The delivery class, as the room client decides it: a broadcast-flag frame addressed to a member goes out like a broadcast at the
    //    radio-like levels (unreliable, once), a unicast one is reliable (level 1) or doubled (level 2, to a plugin that numbers its frames).
    WifiPacket addressed_flag, unicast, relay, broadcast;
    addressed_flag.destination_address = bob.console.mac;
    addressed_flag.broadcast_flag = true;
    unicast.destination_address = bob.console.mac;
    relay.destination_address = kBroadcastMac;
    relay.relayed = true;
    broadcast.destination_address = kBroadcastMac;
    CHECK(IsBroadcastDelivery(addressed_flag) && IsBroadcastDelivery(broadcast) && !IsBroadcastDelivery(unicast) && !IsBroadcastDelivery(relay));
    for (int level = 0; level <= 2; ++level) {
        const FrameDelivery flagged = DeliveryFor(level, false, true, IsBroadcastDelivery(addressed_flag), true);
        const FrameDelivery plain = DeliveryFor(level, false, true, IsBroadcastDelivery(unicast), true);
        CHECK_EQ(flagged.reliable, level == 0);
        CHECK(!flagged.duplicate);
        CHECK_EQ(plain.reliable, level < 2);
        CHECK_EQ(plain.duplicate, level == 2);
    }

    // 6. The node table gives a node two MACs (a host that was in another lobby leaves a stale entry in the node map it sends: the D2 case):
    //    which one is right is not known, so the broadcast, as before. With a clean table (the same packet, one MAC per node) the frame is
    //    addressed again - which also shows that the injected node map is the one the client works from.
    for (Bench* b : {&host, &alice, &bob}) {
        b->e().OnRoomMembers(Members({&host, &alice, &bob}));
    }
    const auto host_node_map = [&](const std::vector<NodeMapEntry>& entries) { // (what the host of the network sends to the room)
        WifiPacket p;
        p.type = WifiPacket::Type::NodeMap;
        p.channel = 11;
        p.transmitter_address = host.console.mac;
        p.destination_address = kBroadcastMac;
        p.data = GenerateNodeMapPacket(entries);
        room.Send(host.console.index, p);
        room.WaitIdle();
    };
    host_node_map({{host.console.mac, 1}, {alice.console.mac, alice_id}, {bob.console.mac, bob_id}});
    std::memset(&g_stats, 0, sizeof(g_stats));
    CHECK_EQ(alice.e().SendTo(bob_id, 1, sizeof(msg), 2, msg, sizeof(msg)), 0u);
    room.WaitIdle();
    CHECK(last_data(alice).destination_address == bob.console.mac);
    CHECK_EQ(g_stats.sendto_addressed, 1u);
    host_node_map({{host.console.mac, 1}, {alice.console.mac, bob_id}, {bob.console.mac, bob_id}});
    std::memset(&g_stats, 0, sizeof(g_stats));
    CHECK_EQ(alice.e().SendTo(bob_id, 1, sizeof(msg), 2, msg, sizeof(msg)), 0u);
    room.WaitIdle();
    CHECK(last_data(alice).destination_address == kBroadcastMac);   // (before the guard: whichever MAC the table listed first)
    CHECK_EQ(g_stats.sendto_addressed, 0u);
    CHECK_RULES();
}

// A client of a 3DS host: the legit traffic of its own lobby is the same through the filter, whoever hosts. (The FFE golden
// replay of Tests/ffe_replay_test runs the whole of a real session with the filter on and off.)
static void TestForeignLobbyLegit() {
    std::printf("foreign lobby: everything a lobby of ours sends passes the filter, in every role\n");
    g_rules.Reset();
    MockRoom room;
    Bench host(room, 1), alice(room, 2), bob(room, 3);
    for (Bench* b : {&host, &alice, &bob}) {
        CHECK_EQ(b->Init(), 0u);
    }
    CHECK_EQ(host.BeginHosting(4), 0u);
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    CHECK_EQ(JoinBench(room, bob, host), 0u);
    for (Bench* b : {&host, &alice, &bob}) {
        b->e().SetClientForeignFilterEnforced(true);
    }
    u32 ev = 0;
    for (Bench* b : {&host, &alice, &bob}) {
        CHECK_EQ(b->e().Bind(2, 0x2E30, 1, kBroadcastNetworkNodeId, &ev), 0u);
    }
    std::memset(&g_stats, 0, sizeof(g_stats));
    const u8 msg[] = "from a node of the lobby";
    // every sender to every receiver, with the broadcast flag (FFE) and with the node's MAC, and to everybody
    int expected[4] = {0, 0, 0, 0};
    Bench* all[3] = {&host, &alice, &bob};
    for (int from = 0; from < 3; ++from) {
        for (int to = 0; to < 3; ++to) {
            if (from == to) {
                continue;
            }
            for (const u8 flags : {u8(2), u8(0)}) {
                CHECK_EQ(all[from]->e().SendTo(static_cast<u16>(to + 1), 1, sizeof(msg), flags, msg, sizeof(msg)), 0u);
                ++expected[to + 1];
            }
        }
        CHECK_EQ(all[from]->e().SendTo(kBroadcastNetworkNodeId, 1, sizeof(msg), 2, msg, sizeof(msg)), 0u);
        for (int to = 0; to < 3; ++to) {
            expected[to + 1] += to != from;
        }
    }
    room.WaitIdle();
    for (int to = 0; to < 3; ++to) {
        Drained d = Drain(all[to]->e(), 2);
        CHECK_EQ(d.foreign, 0);
        CHECK_EQ(d.frames, expected[to + 1]);
    }
    CHECK_EQ(g_stats.drop_foreign, 0u);
    CHECK_EQ(g_stats.foreign_shadow, 0u);
    CHECK_RULES();
}

// The application data of a beacon, and what a client's GetApplicationData answers. TEKKEN 3D PRIME EDITION reads it 100 ms after it
// joins (the log of 2026-10-18: GetApplicationData asked for 0x81 bytes, got 0, and the game left the network 1 ms later, both ways
// between the two consoles). The firmware loads it from the host's current beacon (3dbrew); the old Connect carries 0x3C bytes only.
static std::vector<u8> HexBytes(const char* hex) {
    std::vector<u8> bytes;
    for (; hex[0] && hex[1]; hex += 2) {
        auto nibble = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        bytes.push_back(static_cast<u8>(nibble(hex[0]) << 4 | nibble(hex[1])));
    }
    return bytes;
}

static std::vector<u8> TekkenBeacon() {
    return HexBytes(
    "00e9a435000000006400310400080000000000000000dd07001f32140a0000ddb5001f321500069f10010080000000000002"
    "0200010000000000000000000000a91963669a3a431f9f76331d07de78874eb9279081aa0012000200000000070000000200"
    "0000470069006f006e006b006f00000000000000000000000000000000000000000000000000000000000000000000000000"
    "0102720800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000dd52001f3218c3f7c8c13603fa231bad6529afbab3f5a9467895c4aa1d7bbd1dc12a3cd7"
    "81fab7742d91906851ed398fa8e6fd220eb061c3fef220c6b3bed27e318b1164631cf2dda91034ea861f5a398f29e69c");
}

static void TestBeaconApplicationData() {
    std::printf("Beacon application data: the tag's bytes, and a client's GetApplicationData\n");
    g_rules.Reset();

    // the beacon of Tekken's host as a console received it (298 bytes: its tag of 181 bytes holds 129 of application data)
    const std::vector<u8> tekken = TekkenBeacon();
    CHECK_EQ(tekken.size(), std::size_t(298));
    const u8* data = nullptr;
    u32 size = 0;
    CHECK(GetBeaconApplicationData(FindBeaconTags(tekken), &data, &size));
    CHECK_EQ(size, 0x81u);
    CHECK(data != nullptr && data == tekken.data() + 12 + (2 + 8) + (2 + 7) + 2 + 52); // (header, SSID tag, dummy tag, tag header, 52 bytes)
    CHECK_EQ(data[0], 0xAA);
    CHECK_EQ(data[1], 0x00);
    CHECK_EQ(data[0x11], 'G'); // the host's name, as UTF-16 (little endian)
    CHECK_EQ(data[0x13], 'i');
    // a beacon without the tag, one cut short, and a size byte that says more than the tag holds: nothing
    std::vector<u8> bare(tekken.begin(), tekken.begin() + 12 + 10 + 9);
    CHECK(!GetBeaconApplicationData(FindBeaconTags(bare), &data, &size));
    std::vector<u8> cut = tekken;
    cut.resize(12 + 10 + 9 + 2 + 60); // (the tag says 181 bytes; the vector ends inside it: the tag is not found)
    CHECK(!GetBeaconApplicationData(FindBeaconTags(cut), &data, &size));
    std::vector<u8> lying = tekken;
    lying[12 + 10 + 9 + 2 + 51] = 0xC9; // (more than the tag has, and more than the 0xC8 a network may carry)
    CHECK(!GetBeaconApplicationData(FindBeaconTags(lying), &data, &size));
    lying[12 + 10 + 9 + 2 + 51] = 0xB0; // (fits the limit, not the tag)
    CHECK(!GetBeaconApplicationData(FindBeaconTags(lying), &data, &size));
    std::vector<u8> none = tekken;
    none[12 + 10 + 9 + 2 + 51] = 0; // (a host that never set any: an empty answer that is a real one)
    CHECK(GetBeaconApplicationData(FindBeaconTags(none), &data, &size));
    CHECK_EQ(size, 0u);

    // two consoles: the client joins the way Tekken does (a scan, then the old Connect with the first 0x3C bytes) and asks
    MockRoom room;
    Bench host(room, 1), alice(room, 2);
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(alice.Init(), 0u);
    CHECK_EQ(host.BeginHosting(), 0u); // (6 bytes of application data, 0xA0..0xA5)
    SendBeacon(host);
    room.WaitIdle();
    NetworkInfo net;
    CHECK(alice.ScanFirst(&net));
    CHECK_EQ(alice.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), 0x3C, 1), 0u);
    room.WaitIdle();
    CHECK_EQ(alice.Status().status, 9u);
    u8 app[0xC8];
    u32 app_size = 99;
    CHECK_EQ(alice.e().GetApplicationData(app, sizeof(app), &app_size), 0u);
    CHECK_EQ(app_size, 6u); // (before: 0, and Tekken went away)
    for (int i = 0; i < 6; ++i) {
        CHECK_EQ(app[i], 0xA0 + i);
    }
    CHECK_EQ(alice.e().GetApplicationData(app, 6, &app_size), 0u); // exactly the size
    CHECK_EQ(app_size, 6u);
    CHECK_EQ(alice.e().GetApplicationData(app, 5, &app_size), 0u); // one byte short: the firmware's answer is 0 bytes
    CHECK_EQ(app_size, 0u);

    // the host's data changes: the client reads the new one from the next beacon, and the host reads its own
    const u8 more[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK_EQ(host.e().SetApplicationData(more, sizeof(more)), 0u);
    SendBeacon(host);
    room.WaitIdle();
    CHECK_EQ(alice.e().GetApplicationData(app, sizeof(app), &app_size), 0u);
    CHECK_EQ(app_size, 9u);
    CHECK(std::memcmp(app, more, 9) == 0);
    CHECK_EQ(host.e().GetApplicationData(app, sizeof(app), &app_size), 0u);
    CHECK_EQ(app_size, 9u);
    CHECK(std::memcmp(app, more, 9) == 0);

    // the new Connect hands over the whole NetworkInfo with the data the game scanned (6 bytes, now stale): the beacon still counts
    CHECK_EQ(alice.e().DisconnectNetwork(), 0u);
    room.WaitIdle();
    CHECK_EQ(alice.Status().status, 3u);
    CHECK_EQ(alice.e().GetApplicationData(app, sizeof(app), &app_size), kW); // (no session: the firmware's wrong-state answer)
    CHECK_EQ(app_size, 0u);
    SendBeacon(host);
    room.WaitIdle();
    CHECK(alice.ScanFirst(&net));
    net.application_data_size = 6;
    for (int i = 0; i < 6; ++i) {
        net.application_data[i] = static_cast<u8>(0xA0 + i);
    }
    CHECK_EQ(alice.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1), 0u);
    room.WaitIdle();
    CHECK_EQ(alice.e().GetApplicationData(app, sizeof(app), &app_size), 0u);
    CHECK_EQ(app_size, 9u);
    CHECK(std::memcmp(app, more, 9) == 0);
    CHECK_RULES();
}

static std::vector<u8> LogoffFrom(const Mac& host, u16 assigned, const std::vector<NodeInfo>& nodes, u8 max_nodes, u8 total, WifiPacket* out) {
    out->type = WifiPacket::Type::Data;
    out->channel = 11;
    out->transmitter_address = host;
    out->destination_address = kBroadcastMac;
    out->data = GenerateEapolLogoffFrame(assigned, nodes, max_nodes, total);
    return out->data;
}

// What a game reads back from nwm depends on its role and on what the peers told. The audit of 2026-10-18 (every UDS command against the firmware,
// the real nwm module read for the answers) found the replies below; this test pins them for every role: host, client, spectator, idle.
// ENet as the plugin runs it (the lag analysis of 2026-10-19): the packet throttle never destroys a frame (with ENet's defaults a
// round-trip sample above the estimate lowered it, and at 30/32 one unreliable packet in 16 was destroyed at the sender: Super Street
// Fighter 4 lost 6 % of its frames that way), expendable frames go out UNSEQUENCED, and a pause of the thread that services the host
// (preempted by the game for 20-45 ms) is neither a timeout (no resend, no doubled timeout) nor a round-trip sample.
static void TestEnetThrottleAndPause() {
    std::printf("ENet: the throttle never destroys a frame, expendable frames go unsequenced, a pause is not a timeout\n");
    CHECK_EQ(EnetPacketFlags(true), static_cast<u32>(ENET_PACKET_FLAG_RELIABLE));
    CHECK_EQ(EnetPacketFlags(false), static_cast<u32>(ENET_PACKET_FLAG_UNSEQUENCED | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT));
    CHECK_EQ(enet_initialize(), 0);
    {
        ENetHost* host = enet_host_create(nullptr, 1, 1, 0, 0);
        CHECK(host != nullptr);
        ENetPeer* peer = &host->peers[0];
        peer->lastRoundTripTime = 10;
        peer->lastRoundTripTimeVariance = 2;
        peer->packetThrottle = ENET_PEER_PACKET_THROTTLE_SCALE;
        CHECK_EQ(enet_peer_throttle(peer, 100), -1); // ENet's defaults: a sample above the estimate decelerates
        CHECK_EQ(peer->packetThrottle, 30u);
        peer->packetThrottle = ENET_PEER_PACKET_THROTTLE_SCALE;
        enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL, ENET_PEER_PACKET_THROTTLE_SCALE, 0); // (as RoomClient)
        CHECK_EQ(enet_peer_throttle(peer, 100), -1);
        CHECK_EQ(peer->packetThrottle, 32u); // ... and the plugin's never does
        CHECK_EQ(enet_peer_throttle(peer, 500), -1);
        CHECK_EQ(peer->packetThrottle, 32u);
        enet_host_destroy(host);
    }
    {
        // two hosts on this machine: a sends reliable packets to b
        ENetAddress a_addr{};
        a_addr.host = ENET_HOST_ANY;
        a_addr.port = 42317;
        ENetAddress b_addr{};
        b_addr.host = ENET_HOST_ANY;
        b_addr.port = 42318;
        ENetHost* a = enet_host_create(&a_addr, 1, 1, 0, 0);
        ENetHost* b = enet_host_create(&b_addr, 1, 1, 0, 0);
        CHECK(a != nullptr && b != nullptr);
        ENetAddress to{};
        enet_address_set_host(&to, "127.0.0.1");
        to.port = 42318;
        ENetPeer* pa = enet_host_connect(a, &to, 1, 0);
        CHECK(pa != nullptr);
        ENetPeer* pb = &b->peers[0];
        ENetEvent ev;
        for (int i = 0; i < 400 && !(pa->state == ENET_PEER_STATE_CONNECTED && pb->state == ENET_PEER_STATE_CONNECTED); ++i) {
            if (enet_host_service(a, &ev, 1) > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(ev.packet);
            }
            if (enet_host_service(b, &ev, 1) > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(ev.packet);
            }
        }
        CHECK(pa->state == ENET_PEER_STATE_CONNECTED && pb->state == ENET_PEER_STATE_CONNECTED);
        // a few exchanges: the estimates settle on this machine's round trip (about a millisecond)
        for (int n = 0; n < 5; ++n) {
            enet_peer_send(pa, 0, enet_packet_create("ping", 4, ENET_PACKET_FLAG_RELIABLE));
            for (int i = 0; i < 50; ++i) {
                if (enet_host_service(a, &ev, 1) > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(ev.packet);
                }
                if (enet_host_service(b, &ev, 1) > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(ev.packet);
                }
            }
        }
        CHECK(pa->lastReceiveTime > 0);
        // the thread of `a` is away for 60 ms while b's acknowledgement waits in a's socket: with the timeout of the packet at
        // rtt + 4 var = 5 ms, ENet as shipped would resend it (a loss) and take the 60 ms as a round-trip sample
        pa->roundTripTime = 1;
        pa->roundTripTimeVariance = 1;
        const u32 lost_before = pa->packetsLost;
        enet_peer_send(pa, 0, enet_packet_create("pong", 4, ENET_PACKET_FLAG_RELIABLE));
        enet_host_service(a, &ev, 0); // sent now
        bool received = false;
        for (int i = 0; i < 100 && !received; ++i) {
            if (enet_host_service(b, &ev, 1) > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(ev.packet);
                received = true;
            }
        }
        CHECK(received);
        enet_host_service(b, &ev, 0); // (the acknowledgement is on its way)
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        const u32 skipped_before = enet_azl_ack_samples_skipped;
        enet_host_service(a, &ev, 0); // the socket is read before the timeouts are looked at
        CHECK_EQ(pa->packetsLost, lost_before);                 // no resend
        CHECK(enet_azl_ack_samples_skipped == skipped_before + 1); // the 60 ms were the pause, not the link
        CHECK(pa->roundTripTimeVariance < 10 && pa->roundTripTime < 10);
        CHECK(a->azlServiceGap >= 60);
        enet_peer_disconnect_now(pa, 0);
        enet_host_destroy(a);
        enet_host_destroy(b);
    }
    enet_deinitialize();
}

// ENet's reliable stream with a datagram lost: in order, everything behind the hole waits for the retransmission (hundreds of ms:
// ENet's first timeout); with azlDeliverOutOfOrder the packets behind the hole are handed over at once and the lost one comes
// last, once, when it is resent. A fragmented packet (two datagrams) is handed over whole.
namespace {
// The server's intercept: the datagram that carries the reliable command "p3" (a 2-byte length then the bytes) is swallowed once;
// ENet never sees it.
bool g_enet_dropped = false;
int EnetDropP3(ENetHost* host, ENetEvent*) {
    const u8* d = host->receivedData;
    const std::size_t n = host->receivedDataLength;
    if (g_enet_dropped || n < 4) {
        return 0;
    }
    for (std::size_t i = 0; i + 3 < n; ++i) {
        if (d[i] == 0 && d[i + 1] == 2 && d[i + 2] == 'p' && d[i + 3] == '3') {
            g_enet_dropped = true;
            return 1;
        }
    }
    return 0;
}
// The client's intercept: nothing from the server reaches it (its acknowledgements): what the client sent meanwhile is resent.
int EnetDropAll(ENetHost*, ENetEvent*) {
    return 1;
}
// An early filter that lets only "p4" and "p6" go ahead of a hole.
int EnetEarlyP4P6(const enet_uint8* d, size_t n) {
    return n >= 2 && d[0] == 'p' && (d[1] == '4' || d[1] == '6') ? 1 : 0;
}
} // namespace

static void TestEnetOutOfOrder() {
    std::printf("ENet: a reliable packet behind a lost one is handed over at once with azlDeliverOutOfOrder (and its filter), and only then\n");
    // 0: in order; 1: out of order, then the late copies of what went early; 2: out of order for "p4" and "p6" only; 3: out of
    // order and both hosts destroyed with the placeholders still queued.
    for (int mode = 0; mode < 4; ++mode) {
        ENetAddress address{};
        address.host = ENET_HOST_ANY;
        address.port = 0; // (any free port: the server tells which)
        ENetHost* server = enet_host_create(&address, 1, 2, 0, 0);
        ENetHost* client = enet_host_create(nullptr, 1, 2, 0, 0);
        CHECK(server && client);
        if (!server || !client) {
            return;
        }
        server->azlDeliverOutOfOrder = mode == 0 ? 0u : 1u;
        server->azlEarlyFilter = mode == 2 ? EnetEarlyP4P6 : nullptr;
        ENetAddress to{};
        enet_address_set_host(&to, "127.0.0.1");
        to.port = server->address.port;
        ENetPeer* peer = enet_host_connect(client, &to, 2, 0);
        CHECK(peer != nullptr);
        if (!peer) {
            return;
        }
        bool server_up = false, client_up = false;
        for (int i = 0; i < 200 && !(server_up && client_up); ++i) {
            ENetEvent ev;
            if (enet_host_service(client, &ev, 5) > 0 && ev.type == ENET_EVENT_TYPE_CONNECT) {
                client_up = true;
            }
            if (enet_host_service(server, &ev, 5) > 0 && ev.type == ENET_EVENT_TYPE_CONNECT) {
                server_up = true;
            }
        }
        CHECK(server_up && client_up);
        g_enet_dropped = false;
        server->intercept = EnetDropP3;
        const u32 early_before = enet_azl_early_dispatched;
        std::vector<std::string> got;
        int errors = 0;
        const auto pump = [&](u32 ms) {
            ENetEvent ev;
            const int r = enet_host_service(server, &ev, ms);
            if (r < 0) {
                ++errors;
            } else if (r > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) {
                got.emplace_back(reinterpret_cast<const char*>(ev.packet->data), ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
            }
        };
        const auto names = [&got] {
            std::vector<std::string> n;
            for (const std::string& s : got) {
                n.push_back(s.substr(0, 2));
            }
            return n;
        };
        for (int i = 1; i <= 6; ++i) {
            if (i == 4 && (mode == 1 || mode == 3)) {
                client->intercept = EnetDropAll; // (the acknowledgements of p4..p6 never arrive: the client resends them later, with p3)
            }
            std::string body = "p" + std::to_string(i);
            if (i == 5) {
                body.resize(3000, 'x'); // (three fragments: the whole packet is handed over, in its place)
            }
            ENetPacket* packet = enet_packet_create(body.data(), body.size(), ENET_PACKET_FLAG_RELIABLE);
            enet_peer_send(peer, 0, packet);
            enet_host_flush(client);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            pump(0);
            pump(0);
        }
        for (int i = 0; i < 10; ++i) {
            pump(2);
        }
        // (the client was never serviced: nothing could have been resent yet, whatever the time it took)
        if (mode == 0) {
            CHECK(names() == std::vector<std::string>({"p1", "p2"})); // (everything behind the hole waits)
            CHECK_EQ(enet_azl_early_dispatched - early_before, 0u);
        } else if (mode == 2) {
            CHECK(names() == std::vector<std::string>({"p1", "p2", "p4", "p6"})); // (p5 is not for the early hand-over)
            CHECK_EQ(enet_azl_early_dispatched - early_before, 2u);
        } else {
            CHECK(names() == std::vector<std::string>({"p1", "p2", "p4", "p5", "p6"}));
            CHECK_EQ(enet_azl_early_dispatched - early_before, 3u);
            CHECK(got.size() == 5 && got[3].size() == 3000); // (p5 whole)
        }
        if (mode == 3) {
            // the placeholders are still in the list: both hosts go (enet_peer_reset_queues frees them, nothing is delivered)
            enet_host_destroy(client);
            enet_host_destroy(server);
            continue;
        }
        // the retransmission: p3 comes, once, and in modes 1 the late copies of p4, p5's fragments and p6 (their placeholders) are
        // discarded as duplicates; nothing is delivered again and the sequence accounting goes on
        for (int i = 0; i < 600 && got.size() < 6; ++i) {
            enet_host_service(client, nullptr, 0);
            pump(5);
        }
        client->intercept = nullptr;
        if (mode == 0) {
            CHECK(names() == std::vector<std::string>({"p1", "p2", "p3", "p4", "p5", "p6"}));
        } else if (mode == 2) {
            CHECK(names() == std::vector<std::string>({"p1", "p2", "p4", "p6", "p3", "p5"}));
            CHECK_EQ(enet_azl_early_dispatched - early_before, 2u);
        } else {
            CHECK(names() == std::vector<std::string>({"p1", "p2", "p4", "p5", "p6", "p3"}));
            CHECK_EQ(enet_azl_early_dispatched - early_before, 3u); // (the late copies went early no second time)
        }
        for (int i = 0; i < 20; ++i) { // (the late copies, if any, are in by now: still nothing delivered twice)
            enet_host_service(client, nullptr, 0);
            pump(2);
        }
        CHECK_EQ(got.size(), std::size_t(6));
        // more packets after it: in order again, nothing stuck
        for (int i = 7; i <= 9; ++i) {
            const std::string body = "p" + std::to_string(i);
            enet_peer_send(peer, 0, enet_packet_create(body.data(), body.size(), ENET_PACKET_FLAG_RELIABLE));
            enet_host_flush(client);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            pump(0);
            pump(0);
        }
        for (int i = 0; i < 10; ++i) {
            enet_host_service(client, nullptr, 0);
            pump(2);
        }
        CHECK_EQ(got.size(), std::size_t(9));
        CHECK(got.size() >= 9 && got[6].substr(0, 2) == "p7" && got[8].substr(0, 2) == "p9");
        CHECK_EQ(errors, 0);
        server->intercept = nullptr;
        enet_peer_disconnect_now(peer, 0);
        enet_host_destroy(client);
        enet_host_destroy(server);
    }
}

// The frames SendTo builds are numbered 1, 2, 3 ... (the secure-data header's sequence number, as the real nwm does), and a
// frame that comes again with the number of one delivered lately (the copy a radio_like=2 sender adds) is dropped: by a client
// before its ring, by a host before it forwards. Unnumbered frames (0) are never dropped. A node that leaves is forgotten.
static void TestNumberedFrames() {
    std::printf("numbered frames: SendTo counts, a copy with the same number is dropped once delivered, 0 never is\n");
    g_rules.Reset();
    MockRoom room;
    Bench host(room, 1), alice(room, 2), bob(room, 3);
    for (Bench* b : {&host, &alice, &bob}) {
        CHECK_EQ(b->Init(), 0u);
    }
    CHECK_EQ(host.BeginHosting(4), 0u);
    CHECK_EQ(JoinBench(room, alice, host), 0u);
    CHECK_EQ(JoinBench(room, bob, host), 0u);
    u32 ev = 0;
    for (Bench* b : {&host, &alice, &bob}) {
        CHECK_EQ(b->e().Bind(2, 0x2E30, 1, kBroadcastNetworkNodeId, &ev), 0u);
    }
    room.WaitIdle();
    std::memset(&g_stats, 0, sizeof(g_stats));

    // 1. the numbers: three frames of alice carry 1, 2, 3
    std::vector<u16> numbers;
    std::vector<bool> to_a_plugin; // (the engine's word to the room client: the destination numbers its frames)
    alice.console.sniff = [&numbers, &to_a_plugin](const WifiPacket& p) {
        SecureDataHeader h;
        if (p.type == WifiPacket::Type::Data && ParseSecureDataHeader(p.data, &h)) {
            numbers.push_back(h.sequence_number);
            to_a_plugin.push_back(p.peer_numbers);
        }
    };
    const u8 msg[] = "numbered";
    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(alice.e().SendTo(3, 1, sizeof(msg), 0, msg, sizeof(msg)), 0u);
    }
    room.WaitIdle();
    // (consecutive, never 0; where they start is the engine's clock: a restarted plugin does not collide with its old numbers)
    CHECK(numbers.size() == 3 && numbers[0] != 0 && numbers[1] == static_cast<u16>(numbers[0] + 1) &&
          numbers[2] == static_cast<u16>(numbers[0] + 2));
    CHECK_EQ(Drain(bob.e(), 2).frames, 3);
    // bob has sent alice nothing yet: for all alice knows he may be an Azahar, so no copy for him; after one numbered frame of his
    // she knows he drops a copy
    CHECK(to_a_plugin == std::vector<bool>({false, false, false}));
    CHECK_EQ(bob.e().SendTo(2, 1, sizeof(msg), 0, msg, sizeof(msg)), 0u);
    room.WaitIdle();
    CHECK_EQ(Drain(alice.e(), 2).frames, 1);
    CHECK_EQ(alice.e().SendTo(3, 1, sizeof(msg), 0, msg, sizeof(msg)), 0u);
    CHECK_EQ(alice.e().SendTo(kBroadcastNetworkNodeId, 1, sizeof(msg), 2, msg, sizeof(msg)), 0u); // (a broadcast: never a copy)
    room.WaitIdle();
    CHECK(to_a_plugin.size() == 5 && to_a_plugin[3] && !to_a_plugin[4]);
    Drain(bob.e(), 2);
    Drain(host.e(), 2);
    alice.console.sniff = nullptr;

    // 2. bob gets the same numbered frame twice (the copy): once in his ring; the next number goes; 0 twice goes twice
    const auto frame_from = [&](Bench& from, u16 dest, u16 seq, const Mac& to) {
        WifiPacket p;
        p.type = WifiPacket::Type::Data;
        p.channel = 11;
        p.transmitter_address = from.console.mac;
        p.destination_address = to;
        p.data = GenerateDataPayload(msg, sizeof(msg), 1, dest, from.Status().network_node_id, seq);
        return p;
    };
    bob.console.engine->OnWifiPacket(frame_from(alice, 3, 7, bob.console.mac));
    bob.console.engine->OnWifiPacket(frame_from(alice, 3, 7, bob.console.mac));
    CHECK_EQ(Drain(bob.e(), 2).frames, 1);
    CHECK_EQ(g_stats.drop_dup_copy, 1u);
    bob.console.engine->OnWifiPacket(frame_from(alice, 3, 8, bob.console.mac));
    CHECK_EQ(Drain(bob.e(), 2).frames, 1);
    bob.console.engine->OnWifiPacket(frame_from(alice, 3, 0, bob.console.mac));
    bob.console.engine->OnWifiPacket(frame_from(alice, 3, 0, bob.console.mac));
    CHECK_EQ(Drain(bob.e(), 2).frames, 2);
    CHECK_EQ(g_stats.drop_dup_copy, 1u);
    // a frame for bob through the broadcast MAC, numbered, then its copy: the twin rule has nothing to pair, the copy rule drops it
    bob.console.engine->OnWifiPacket(frame_from(alice, 3, 9, kBroadcastMac));
    bob.console.engine->OnWifiPacket(frame_from(alice, 3, 9, kBroadcastMac));
    CHECK_EQ(Drain(bob.e(), 2).frames, 1);
    CHECK_EQ(g_stats.drop_dup_copy, 2u);

    // 3. the host forwards a frame of alice for bob once, whatever copies of it come to its MAC
    std::memset(&g_stats, 0, sizeof(g_stats));
    host.console.engine->OnWifiPacket(frame_from(alice, 3, 20, host.console.mac));
    host.console.engine->OnWifiPacket(frame_from(alice, 3, 20, host.console.mac));
    room.WaitIdle();
    CHECK_EQ(g_stats.forwarded, 1u);
    CHECK_EQ(g_stats.drop_dup_copy, 1u);
    CHECK_EQ(Drain(bob.e(), 2).frames, 1); // (the one forwarded copy)

    // 4. alice leaves; the next console that gets her node id starts from 1 again, and the host takes its number 20
    CHECK_EQ(alice.e().DisconnectNetwork(), 0u);
    room.WaitIdle();
    Bench carol(room, 4);
    CHECK_EQ(carol.Init(), 0u);
    CHECK_EQ(JoinBench(room, carol, host), 0u);
    room.WaitIdle();
    CHECK_EQ(carol.Status().network_node_id, 2); // (alice's id)
    std::memset(&g_stats, 0, sizeof(g_stats));
    host.console.engine->OnWifiPacket(frame_from(carol, 3, 20, host.console.mac));
    room.WaitIdle();
    CHECK_EQ(g_stats.forwarded, 1u);
    CHECK_EQ(g_stats.drop_dup_copy, 0u);
    CHECK_RULES();
}

static void TestRepliesByRole() {
    std::printf("replies by role: node table, changed nodes, application data, channel, joins in one round trip\n");
    g_rules.Reset();
    MockRoom room;
    RawClient spy(room, 9); // hears every broadcast
    Bench host(room, 1), alice(room, 2), bob(room, 3);
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(alice.Init(), 0u);
    CHECK_EQ(bob.Init(), 0u);

    auto zero_node = [](Bench& b, u16 id) {
        NodeInfo n;
        n.friend_code_seed = 0xFFFF;
        n.username[0] = 0xFFFF;
        n.network_node_id = 0xFFFF;
        const u32 r = b.e().GetNodeInformation(id, &n);
        return r == 0u && n.friend_code_seed == 0 && n.username[0] == 0 && n.network_node_id == 0;
    };
    auto node_of = [](Bench& b, u16 id, NodeInfo* n) { return b.e().GetNodeInformation(id, n); };

    // idle: the table is empty, and an empty slot is id 0 (nwm's 16 slots): id 0 is found, as zeros, in every state
    CHECK(zero_node(alice, 0));
    NodeInfo n;
    CHECK_EQ(node_of(alice, 1, &n), kNodeNotFound);
    CHECK_EQ(node_of(alice, 17, &n), kNodeNotFound);
    CHECK_EQ(node_of(alice, 0xFFFF, &n), kNodeNotFound);
    u8 app[0xC8];
    u32 app_size = 99;
    CHECK_EQ(alice.e().GetApplicationData(app, sizeof(app), &app_size), kW);
    CHECK_EQ(app_size, 0u);

    // host alone
    CHECK_EQ(host.BeginHosting(3), 0u);
    CHECK(zero_node(host, 0));
    CHECK_EQ(node_of(host, 1, &n), 0u);
    CHECK_EQ(n.friend_code_seed, 0x1001u);
    CHECK_EQ(n.username[0], 'B');
    CHECK_EQ(node_of(host, 2, &n), kNodeNotFound);
    u8 channel = 0;
    CHECK_EQ(host.e().GetChannel(&channel), 0u);
    CHECK_EQ(channel, 11);
    CHECK_EQ(host.e().GetApplicationData(app, sizeof(app), &app_size), 0u);
    CHECK_EQ(app_size, 6u);

    // a client (the old Connect, the 0x3C bytes only)
    SendBeacon(host);
    room.WaitIdle();
    NetworkInfo net;
    CHECK(alice.ScanFirst(&net));
    CHECK_EQ(alice.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), 0x3C, 1), 0u);
    room.WaitIdle();
    CHECK(zero_node(alice, 0));
    CHECK_EQ(node_of(alice, 1, &n), 0u);
    CHECK_EQ(n.friend_code_seed, 0x1001u); // the host, from the Logoff
    CHECK_EQ(node_of(alice, 2, &n), 0u);
    CHECK_EQ(n.friend_code_seed, 0x1002u); // itself
    CHECK_EQ(node_of(alice, 3, &n), kNodeNotFound);
    CHECK_EQ(node_of(alice, 17, &n), kNodeNotFound);
    CHECK_EQ(alice.e().GetChannel(&channel), 0u);
    CHECK_EQ(channel, 11);
    CHECK_EQ(alice.e().GetApplicationData(app, sizeof(app), &app_size), 0u);
    CHECK_EQ(app_size, 6u);
    CHECK_EQ(alice.Status().network_node_id, 2);

    // a spectator joins the same host: node id 0, the host's data from the beacon, the lobby it joined
    SendBeacon(host);
    room.WaitIdle();
    CHECK(bob.ScanFirst(&net));
    CHECK_EQ(bob.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), 0x3C, 2), 0u);
    room.WaitIdle();
    CHECK_EQ(bob.Status().status, 10u);
    CHECK(zero_node(bob, 0));
    CHECK_EQ(node_of(bob, 1, &n), 0u);
    CHECK_EQ(n.friend_code_seed, 0x1001u);
    CHECK_EQ(bob.e().GetApplicationData(app, sizeof(app), &app_size), 0u);
    CHECK_EQ(app_size, 6u);
    CHECK_EQ(bob.e().GetChannel(&channel), 0u);
    CHECK_EQ(channel, 11);

    // a full lobby (2 of 2): id 0 is still the empty slot (the table here is as long as the lobby: it used to say "not found")
    {
        Bench full(room, 5), dave(room, 6);
        CHECK_EQ(full.Init(), 0u);
        CHECK_EQ(dave.Init(), 0u);
        CHECK_EQ(full.BeginHosting(2), 0u);
        CHECK_EQ(JoinBench(room, dave, full), 0u);
        CHECK_EQ(full.Status().total_nodes, 2);
        CHECK(zero_node(full, 0));
        CHECK(zero_node(dave, 0));
    }
    CHECK(zero_node(host, 0));
    CHECK_EQ(host.e().DisconnectNetwork(), kW);
    CHECK_EQ(host.e().DestroyNetwork(), 0u);
    CHECK(zero_node(host, 0));
    room.WaitIdle();

    // hosting with channel 0 after a network on channel 6: the channel is the default one again, for GetChannel and for the beacons
    NetworkInfo six = MakeNetworkInfo(3, 6);
    six.channel = 6;
    CHECK_EQ(host.e().BeginHostingNetwork(reinterpret_cast<u8*>(&six), sizeof(six)), 0u);
    CHECK_EQ(host.e().GetChannel(&channel), 0u);
    CHECK_EQ(channel, 6);
    CHECK_EQ(host.e().DestroyNetwork(), 0u);
    CHECK_EQ(host.BeginHosting(3), 0u); // channel 0
    CHECK_EQ(host.e().GetChannel(&channel), 0u);
    CHECK_EQ(channel, 11);
    spy.Take();
    SendBeacon(host);
    room.WaitIdle();
    int beacons = 0;
    for (const WifiPacket& p : spy.Take()) {
        if (p.type == WifiPacket::Type::Beacon && p.transmitter_address == host.console.mac) {
            ++beacons;
            CHECK_EQ(p.channel, 11);
        }
    }
    CHECK(beacons >= 1);

    // SetApplicationData with size 0 clears what the lobby advertises (the firmware's only rule is size <= 0xC8)
    const u8 none = 0;
    CHECK_EQ(host.e().SetApplicationData(&none, 0), 0u);
    CHECK_EQ(host.e().GetApplicationData(app, sizeof(app), &app_size), 0u);
    CHECK_EQ(app_size, 0u);
    CHECK_RULES();
}

// Two joins in one round trip: the host broadcasts the Logoff of each admission, and a client that is still Connecting must take the one that
// names ITS node, not the first it hears (before, both adopted the first one's id and the lobby never formed: the symptom Azahar has too).
static void TestJoinsInOneRoundTrip() {
    std::printf("a client that is still connecting takes its own Logoff, not the one of another join\n");
    g_rules.Reset();
    MockRoom room;
    Bench alice(room, 2);
    CHECK_EQ(alice.Init(), 0u);
    alice.kernel.cap_ns = 5000000000LL; // the join stays open while the test injects the frames
    const Mac X{0x00, 0x1F, 0x32, 0x00, 0x00, 0x66}; // the host: nobody answers, the test speaks for it
    g_rules.AddMember(X);
    NetworkInfo net = MakeNetworkInfo(4, 0);
    net.host_mac_address = X;
    net.channel = 11;
    std::atomic<bool> returned{false};
    std::atomic<u32> result{0xFFFFFFFF};
    std::thread t([&] {
        result = alice.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), 0x3C, 1);
        returned = true;
    });
    auto spin = [](const std::function<bool()>& done) {
        for (int i = 0; i < 5000 && !done(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return done();
    };
    CHECK(spin([&] { return alice.e().status() == NetworkStatus::Connecting; }));

    std::vector<NodeInfo> nodes(4);
    nodes[0] = MakeNode(0x1111, "AzHost", 1);
    nodes[1] = MakeNode(0x2222, "Other", 2);
    WifiPacket foreign;
    LogoffFrom(X, 2, nodes, 4, 2, &foreign); // somebody else's admission: slot 2 holds another console
    alice.e().OnWifiPacket(foreign);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(alice.e().status() == NetworkStatus::Connecting);
    CHECK(!returned.load());

    nodes[2] = MakeNode(0x1002, "Bench", 3); // ours: the seed and the name Alice joined with
    WifiPacket mine;
    LogoffFrom(X, 3, nodes, 4, 3, &mine);
    alice.e().OnWifiPacket(mine);
    CHECK(spin([&] { return returned.load(); }));
    t.join();
    CHECK_EQ(result.load(), 0u);
    ConnectionStatus st = alice.Status();
    CHECK_EQ(st.status, 9u);
    CHECK_EQ(st.network_node_id, 3);
    CHECK_EQ(st.node_bitmask, 7);
    NodeInfo n;
    CHECK_EQ(alice.e().GetNodeInformation(3, &n), 0u);
    CHECK_EQ(n.friend_code_seed, 0x1002u);

    // a Logoff that does not list the slot it names is taken as before (a host that sends no node list must not leave a client waiting)
    Bench carol(room, 4);
    CHECK_EQ(carol.Init(), 0u);
    carol.kernel.cap_ns = 5000000000LL;
    NetworkInfo net2 = net;
    std::atomic<bool> returned2{false};
    std::thread t2([&] {
        carol.e().ConnectToNetwork(reinterpret_cast<u8*>(&net2), 0x3C, 1);
        returned2 = true;
    });
    CHECK(spin([&] { return carol.e().status() == NetworkStatus::Connecting; }));
    std::vector<NodeInfo> only_host(4);
    only_host[0] = MakeNode(0x1111, "AzHost", 1);
    WifiPacket unlisted;
    LogoffFrom(X, 2, only_host, 4, 2, &unlisted);
    carol.e().OnWifiPacket(unlisted);
    CHECK(spin([&] { return returned2.load(); }));
    t2.join();
    CHECK_EQ(carol.Status().network_node_id, 2);

    // an EAPoL-Start of a type nobody knows admits nobody and is not answered with a Logoff (id 0 would be adopted by every connecting client)
    Bench host(room, 1);
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(host.BeginHosting(4), 0u);
    RawClient r(room, 7);
    r.Auth(host.console.mac);
    room.WaitIdle();
    r.Take();
    const u32 events = host.Signals(host.status_event);
    r.Send(WifiPacket::Type::Data, host.console.mac,
           GenerateEapolStartFrame(1, static_cast<ConnectionType>(7), MakeNode(0x7000 + r.mac[5], "Raw", 0)));
    room.WaitIdle();
    CHECK_EQ(r.Logoffs(true), 0);
    CHECK_EQ(r.Logoffs(false), 0);
    CHECK_EQ(host.Status().total_nodes, 1);
    CHECK_EQ(host.Signals(host.status_event), events);
    CHECK_RULES();
}

// Two joins before the game reads the status: changed_nodes keeps both bits (nwm ORs the changes in; the one-shot status event wakes the game once).
static void TestChangedNodesAccumulate() {
    std::printf("a client's changed_nodes accumulates until the game reads it\n");
    g_rules.Reset();
    MockRoom room;
    NetworkInfo ni = MakeNetworkInfo(4, 6);
    AzaharHost azhost(room, 1, ni, MakeNode(0x1111, "AzHost", 0));
    Bench alice(room, 2);
    CHECK_EQ(alice.Init(), 0u);
    const Mac H = azhost.mac();
    azhost.SendBeacon();
    room.WaitIdle();
    NetworkInfo net;
    CHECK(alice.ScanFirst(&net));
    CHECK_EQ(alice.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), sizeof(net), 1), 0u);
    room.WaitIdle();
    ConnectionStatus st = alice.Status(); // (read: the bits of the join are gone)
    CHECK_EQ(st.node_bitmask, 3);
    RawClient b(room, 3), c(room, 4);
    b.Auth(H);
    b.Start(H);
    c.Auth(H);
    c.Start(H);
    room.WaitIdle();
    st = alice.Status();
    CHECK_EQ(st.node_bitmask, 0x000F);
    CHECK_EQ(st.changed_nodes, 0x000C); // both joins (before: only the bit of the second refresh)
    CHECK_EQ(alice.Status().changed_nodes, 0);
    CHECK_RULES();
}

// The beacon of the host a client joined is the one GetApplicationData reads: with the table of heard beacons full of other lobbies it is kept.
static void TestHostBeaconKept() {
    std::printf("the beacon of a client's host is not the one that makes room for others\n");
    g_rules.Reset();
    MockRoom room;
    RawClient spy(room, 9);
    Bench host(room, 1), alice(room, 2);
    CHECK_EQ(host.Init(), 0u);
    CHECK_EQ(alice.Init(), 0u);
    // Tekken's host: the 129 bytes its beacon carried (cut from the capture of 2026-10-18), 0x3C bytes of Connect, GetApplicationData(0x81)
    const std::vector<u8> tekken = TekkenBeacon();
    NetworkInfo ni = MakeNetworkInfo(2, 0);
    ni.application_data_size = 0x81;
    std::memcpy(ni.application_data, tekken.data() + 85, 0x81);
    CHECK_EQ(host.e().BeginHostingNetwork(reinterpret_cast<u8*>(&ni), sizeof(ni)), 0u);
    SendBeacon(host);
    room.WaitIdle();
    spy.Take();
    SendBeacon(host);
    room.WaitIdle();
    WifiPacket host_beacon;
    for (const WifiPacket& p : spy.Take()) {
        if (p.type == WifiPacket::Type::Beacon) {
            host_beacon = p;
        }
    }
    CHECK(host_beacon.type == WifiPacket::Type::Beacon);
    NetworkInfo net;
    CHECK(alice.ScanFirst(&net));
    CHECK_EQ(alice.e().ConnectToNetwork(reinterpret_cast<u8*>(&net), 0x3C, 1), 0u);
    room.WaitIdle();
    u8 app[0xC8];
    u32 app_size = 0;
    CHECK_EQ(alice.e().GetApplicationData(app, 0x81, &app_size), 0u);
    CHECK_EQ(app_size, 0x81u);
    CHECK(std::memcmp(app, tekken.data() + 85, 0x81) == 0);
    // twenty other lobbies are heard after it: the host's entry is the oldest, and stays
    for (int i = 0; i < 20; ++i) {
        WifiPacket other = host_beacon;
        other.transmitter_address = Mac{0x00, 0x1F, 0x32, 0x00, 0x00, static_cast<u8>(0x50 + i)};
        alice.e().OnWifiPacket(other);
    }
    app_size = 0;
    CHECK_EQ(alice.e().GetApplicationData(app, 0x81, &app_size), 0u);
    CHECK_EQ(app_size, 0x81u);
    CHECK(std::memcmp(app, tekken.data() + 85, 0x81) == 0);
    CHECK_RULES();
}

int main() {
    SetLogSink([](const char*) {}); // quiet
    TestNdmFfeSequence();
    TestNdmExclusiveState();
    TestNdmHeaders();
    TestNdmPidWords();
    TestNdmDaemonsAndScheduler();
    TestNdmSessions();
    TestCrypto();
    TestProto();
    TestFrames();
    TestEngine();
    TestConnectTimeout();
    TestAzaharRules();
    TestAzaharHostModel();
    TestDebugCapture();
    TestCaptureOfDroppedFrames();
    TestNodeMapLines();
    TestRoleLines();
    TestEngineBeforeInitialize();
    TestInitializeSemantics();
    TestShutdownSemantics();
    TestDestroyDisconnect();
    TestBeaconApplicationData();
    TestRepliesByRole();
    TestEnetThrottleAndPause();
    TestEnetOutOfOrder();
    TestNumberedFrames();
    TestJoinsInOneRoundTrip();
    TestChangedNodesAccumulate();
    TestHostBeaconKept();
    TestConnectArguments();
    TestEjectClient();
    TestSendToSemantics();
    TestNetworkAttributes();
    TestServiceStop();
    TestConnectGivenUp();
    TestConnectWokenLate();
    TestHostBeaconAge();
    TestScanFidelity();
    TestHostReauth();
    TestRepeatedStartDoesNotAdmitOthers();
    TestRepeatedStartInClosedLobby();
    TestClientHonoursHostDeauth();
    TestSpectatorLifecycle();
    TestPreDeauth();
    TestHostForwardRule();
    TestBeaconSyncShadow();
    TestTicks();
    TestTxQueue();
    TestEncodeAllocations();
    TestContendedMutex();
    TestSignalsAfterUnlock();
    TestTickWithoutLock();
    TestServiceTimeoutRule();
    TestStatsSinks();
    TestPrefilterEquivalence();
    TestEngineEventCounters();
    TestOwnNodeInfoSeed();
    TestTransportDown();
    TestRoomPresence();
    TestRoomMemberStopsPlaying();
    TestBeaconSchedule();
    TestLateSecondDeauth();
    TestForeignLobbyHost();
    TestForeignLobbyClient();
    TestForeignLobbyLegit();
    TestAddressedBroadcastFlag();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
