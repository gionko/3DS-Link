// Azahar/Citra multiplayer room protocol (network version 4): packet encoding only.
// Wire format = SFML-style sf::Packet: big-endian integers, strings and vectors are prefixed
// with a u32 length.
#pragma once

#include <string>
#include <vector>

#include "azl/common.hpp"
#include "azl/uds_frames.hpp"

namespace azl {
namespace room {

constexpr u32 kNetworkVersion = 4;
constexpr u16 kDefaultPort = 24872;
constexpr std::size_t kNumChannels = 1;

enum MessageId : u8 {
    IdJoinRequest = 1,
    IdJoinSuccess,
    IdRoomInformation,
    IdSetGameInfo,
    IdWifiPacket,
    IdChatMessage,
    IdNameCollision,
    IdMacCollision,
    IdVersionMismatch,
    IdWrongPassword,
    IdCloseRoom,
    IdRoomIsFull,
    IdConsoleIdCollision,
    IdStatusMessage,
    IdHostKicked,
    IdHostBanned,
    IdModKick,
    IdModBan,
    IdModUnban,
    IdModGetBanList,
    IdModBanListResponse,
    IdModPermissionDenied,
    IdModNoSuchUser,
    IdJoinSuccessAsMod,
};

class PacketWriter {
public:
    /// One allocation instead of a dozen reallocations (each a malloc, a copy and a free) while a packet grows.
    void Reserve(std::size_t n) {
        buf_.reserve(n);
    }
    void U8(u8 v) {
        buf_.push_back(v);
    }
    void U16(u16 v) {
        u8 b[2];
        StoreBE16(b, v);
        buf_.insert(buf_.end(), b, b + 2);
    }
    void U32(u32 v) {
        u8 b[4];
        StoreBE32(b, v);
        buf_.insert(buf_.end(), b, b + 4);
    }
    void U64(u64 v) {
        u8 b[8];
        StoreBE64(b, v);
        buf_.insert(buf_.end(), b, b + 8);
    }
    void String(const std::string& s) {
        U32(static_cast<u32>(s.size()));
        buf_.insert(buf_.end(), s.begin(), s.end());
    }
    void MacAddress(const Mac& m) {
        buf_.insert(buf_.end(), m.begin(), m.end());
    }
    void ByteVector(const std::vector<u8>& v) {
        U32(static_cast<u32>(v.size()));
        buf_.insert(buf_.end(), v.begin(), v.end());
    }
    const std::vector<u8>& data() const {
        return buf_;
    }
    std::vector<u8> Take() {
        return std::move(buf_);
    }

private:
    std::vector<u8> buf_;
};

class PacketReader {
public:
    PacketReader(const u8* data, std::size_t size) : data_(data), size_(size) {}

    bool ok() const {
        return ok_;
    }
    void Skip(std::size_t n) {
        if (Check(n)) {
            pos_ += n;
        }
    }
    u8 U8() {
        return Check(1) ? data_[pos_++] : 0;
    }
    u16 U16() {
        if (!Check(2)) {
            return 0;
        }
        const u16 v = LoadBE16(data_ + pos_);
        pos_ += 2;
        return v;
    }
    u32 U32() {
        if (!Check(4)) {
            return 0;
        }
        const u32 v = LoadBE32(data_ + pos_);
        pos_ += 4;
        return v;
    }
    u64 U64() {
        if (!Check(8)) {
            return 0;
        }
        const u64 v = LoadBE64(data_ + pos_);
        pos_ += 8;
        return v;
    }
    std::string String() {
        const u32 len = U32();
        std::string s;
        if (len > 0 && Check(len)) {
            s.assign(reinterpret_cast<const char*>(data_ + pos_), len);
            pos_ += len;
        }
        return s;
    }
    Mac MacAddress() {
        Mac m{};
        if (Check(6)) {
            std::memcpy(m.data(), data_ + pos_, 6);
            pos_ += 6;
        }
        return m;
    }
    std::vector<u8> ByteVector() {
        const u32 len = U32();
        std::vector<u8> v;
        if (len > 0 && Check(len)) {
            v.assign(data_ + pos_, data_ + pos_ + len);
            pos_ += len;
        }
        return v;
    }

private:
    bool Check(std::size_t n) {
        ok_ = ok_ && (size_ - pos_ >= n);
        return ok_;
    }
    const u8* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

struct MemberInfo {
    std::string nickname;
    Mac mac{};
    std::string game_name;
    u64 game_id = 0;
    std::string username;
    std::string display_name;
    std::string avatar_url;
};

struct RoomInfo {
    std::string name;
    std::string description;
    u32 member_slots = 0;
    u16 port = 0;
    std::string preferred_game;
    std::string host_username;
    std::vector<MemberInfo> members;
};

std::vector<u8> EncodeJoinRequest(const std::string& nickname, const std::string& console_id_hash,
                                  const Mac& preferred_mac, const std::string& password,
                                  const std::string& token);
std::vector<u8> EncodeSetGameInfo(const std::string& name, u64 id);
std::vector<u8> EncodeWifiPacket(const WifiPacket& packet);

/// `data` is the whole ENet packet including the leading message id byte.
bool DecodeWifiPacket(const u8* data, std::size_t size, WifiPacket* out);
bool DecodeRoomInformation(const u8* data, std::size_t size, RoomInfo* out);
bool DecodeJoinSuccess(const u8* data, std::size_t size, Mac* assigned_mac);

} // namespace room
} // namespace azl
