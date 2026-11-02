#include "azl/room_proto.hpp"

namespace azl {
namespace room {

std::vector<u8> EncodeJoinRequest(const std::string& nickname, const std::string& console_id_hash,
                                  const Mac& preferred_mac, const std::string& password,
                                  const std::string& token) {
    PacketWriter w;
    w.U8(IdJoinRequest);
    w.String(nickname);
    w.String(console_id_hash);
    w.MacAddress(preferred_mac);
    w.U32(kNetworkVersion);
    w.String(password);
    w.String(token);
    return w.Take();
}

std::vector<u8> EncodeSetGameInfo(const std::string& name, u64 id) {
    PacketWriter w;
    w.U8(IdSetGameInfo);
    w.String(name);
    w.U64(id);
    return w.Take();
}

std::vector<u8> EncodeChat(const std::string& message) {
    PacketWriter w;
    w.U8(IdChatMessage);
    w.String(message.size() > kMaxChatChars ? message.substr(0, kMaxChatChars) : message);
    return w.Take();
}

bool DecodeChat(const u8* data, std::size_t size, std::string* nickname, std::string* message) {
    PacketReader r(data, size);
    if (r.U8() != IdChatMessage) {
        return false;
    }
    std::string nick = r.String();
    r.String(); // the username of the account (not shown)
    std::string text = r.String();
    if (!r.ok()) {
        return false;
    }
    if (nickname) {
        *nickname = std::move(nick);
    }
    if (message) {
        *message = std::move(text);
    }
    return true;
}

std::vector<u8> EncodeWifiPacket(const WifiPacket& packet) {
    PacketWriter w;
    w.Reserve(1 + 1 + 1 + 6 + 6 + 4 + packet.data.size());
    w.U8(IdWifiPacket);
    w.U8(static_cast<u8>(packet.type));
    w.U8(packet.channel);
    w.MacAddress(packet.transmitter_address);
    w.MacAddress(packet.destination_address);
    w.ByteVector(packet.data);
    return w.Take();
}

bool DecodeWifiPacket(const u8* data, std::size_t size, WifiPacket* out) {
    PacketReader r(data, size);
    r.Skip(1); // message id
    const u8 type = r.U8();
    out->channel = r.U8();
    out->transmitter_address = r.MacAddress();
    out->destination_address = r.MacAddress();
    out->data = r.ByteVector();
    if (!r.ok() || type > static_cast<u8>(WifiPacket::Type::NodeMap)) {
        return false;
    }
    out->type = static_cast<WifiPacket::Type>(type);
    return true;
}

bool DecodeRoomInformation(const u8* data, std::size_t size, RoomInfo* out) {
    PacketReader r(data, size);
    r.Skip(1);
    out->name = r.String();
    out->description = r.String();
    out->member_slots = r.U32();
    out->port = r.U16();
    out->preferred_game = r.String();
    out->host_username = r.String();
    const u32 count = r.U32();
    out->members.clear();
    // Each member needs at least 6 (mac) + 5*4 (string lengths) + 8 (game id) bytes.
    if (!r.ok() || count > size / 34) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        MemberInfo m;
        m.nickname = r.String();
        m.mac = r.MacAddress();
        m.game_name = r.String();
        m.game_id = r.U64();
        m.username = r.String();
        m.display_name = r.String();
        m.avatar_url = r.String();
        if (!r.ok()) {
            return false;
        }
        out->members.push_back(std::move(m));
    }
    return true;
}

bool DecodeJoinSuccess(const u8* data, std::size_t size, Mac* assigned_mac) {
    PacketReader r(data, size);
    r.Skip(1);
    *assigned_mac = r.MacAddress();
    return r.ok();
}

} // namespace room
} // namespace azl
