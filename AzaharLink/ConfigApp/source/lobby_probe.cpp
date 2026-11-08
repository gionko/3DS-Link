#include "lobby_probe.hpp"

#include <cstdint>

#include "azl/platform.hpp"
#include "azl/room_proto.hpp"
#include "enet/enet.h"

namespace lobbies {

namespace {

namespace room = azl::room;

constexpr std::uint32_t kNoVersion = 0xFFFFFFFFu; // (no room speaks it: the join request is always refused, and says why)

void Send(ENetPeer* peer, const std::vector<azl::u8>& bytes) {
    ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
    if (packet && enet_peer_send(peer, 0, packet) != 0) {
        enet_packet_destroy(packet);
    }
}

// A join request the room answers without taking us in: no nickname, no MAC, a version that cannot be its own.
std::vector<azl::u8> AskingRequest(const std::string& password) {
    room::PacketWriter w;
    w.U8(room::IdJoinRequest);
    w.String("3DSLink-probe");
    w.String("3dslink-probe");
    w.MacAddress(azl::kNoPreferredMac);
    w.U32(kNoVersion);
    w.String(password);
    w.String("");
    return w.Take();
}

} // namespace

ProbePass::ProbePass(std::vector<Target> targets, PassOptions options) : options_(options) {
    slots_.resize(targets.size());
    for (std::size_t i = 0; i < targets.size(); ++i) {
        slots_[i].target = targets[i];
    }
    if (options_.at_once < 1) {
        options_.at_once = 1;
    }
}

ProbePass::~ProbePass() {
    if (host_) {
        for (Slot& s : slots_) {
            if (s.peer) {
                s.peer->data = nullptr;
                // (a pass stopped half way: the rooms hear that we left, also the ones still answering our connect, and no place stays taken)
                enet_peer_disconnect_now(s.peer, 0);
                s.peer = nullptr;
            }
        }
        enet_host_flush(host_);
        enet_host_destroy(host_);
    }
    if (enet_ready_) {
        enet_deinitialize();
    }
}

bool ProbePass::Begin() {
    if (enet_initialize() != 0) {
        return false;
    }
    enet_ready_ = true;
    host_ = enet_host_create(nullptr, static_cast<std::size_t>(options_.at_once), room::kNumChannels, 0, 0);
    if (host_) {
        // (as the room client's: a room that sends something huge is refused, not kept in memory)
        host_->maximumPacketSize = 64 * 1024;
        host_->maximumWaitingData = 256 * 1024;
    }
    return host_ != nullptr;
}

void ProbePass::Finish(Slot& s, State state) {
    if (s.peer) {
        s.peer->data = nullptr;
        if (state == State::Online) {
            enet_peer_disconnect_now(s.peer, 0); // (the room hears that we left: it frees the slot at once)
        } else {
            enet_peer_reset(s.peer);
        }
        s.peer = nullptr;
    }
    s.probe.state = state;
    s.done = true;
    s.changed = true;
    --asking_;
}

void ProbePass::StartNext(std::uint64_t now) {
    while (asking_ < options_.at_once && next_ < slots_.size() && !(options_.stop && options_.stop->load())) {
        Slot& s = slots_[next_++];
        ENetAddress address{};
        // (a name is looked up here, on the thread of the pass: the lists hold addresses, so it is quick)
        if (enet_address_set_host(&address, s.target.host.c_str()) != 0) {
            ++asking_;
            Finish(s, State::Offline);
            continue;
        }
        address.port = s.target.port;
        s.peer = enet_host_connect(host_, &address, room::kNumChannels, 0);
        if (!s.peer) {
            --next_; // (no free peer: a room that just ended still holds its place for a moment)
            return;
        }
        s.peer->data = &s;
        s.started_ms = azl::platform::NowMs(); // (after the name was looked up: the ping is the room's, not the lookup's)
        (void)now;
        s.probe.state = State::Asking;
        s.changed = true;
        ++asking_;
    }
}

void ProbePass::OnConnect(Slot& s, std::uint64_t now) {
    if (!s.peer || s.done) {
        return;
    }
    s.connected_ms = now;
    s.probe.ping_ms = static_cast<int>(now - s.started_ms);
    s.probe.state = State::Online;
    s.changed = true;
    Send(s.peer, room::EncodeSetGameInfo("", 0));
    Send(s.peer, AskingRequest(""));
}

void ProbePass::OnPacket(Slot& s, const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return;
    }
    s.changed = true;
    switch (data[0]) {
    case room::IdRoomInformation: {
        room::RoomInfo info;
        if (room::DecodeRoomInformation(data, size, &info)) {
            s.probe.has_info = true;
            s.probe.name = info.name;
            s.probe.description = info.description;
            s.probe.slots = info.member_slots;
            s.probe.members.clear();
            for (const room::MemberInfo& m : info.members) {
                Member member;
                member.nickname = m.nickname;
                member.game = m.game_name;
                member.game_id = m.game_id;
                s.probe.members.push_back(member);
            }
        }
        break;
    }
    case room::IdRoomIsFull:
        s.probe.full = true;
        s.answered = true;
        break;
    case room::IdWrongPassword:
        if (!s.second_ask) {
            s.probe.lock = Lock::Password;
            if (!s.target.secret.empty()) {
                s.second_ask = true; // (is ours the right one?)
                s.asked_ms = azl::platform::NowMs();
                Send(s.peer, AskingRequest(s.target.secret));
                break;
            }
        }
        s.probe.secret_ok = false;
        s.answered = true;
        break;
    case room::IdVersionMismatch: {
        room::PacketReader r(data, size);
        r.U8();
        const std::uint32_t version = r.U32();
        if (r.ok()) {
            s.probe.version = version;
        }
        if (s.second_ask) {
            s.probe.secret_ok = true;
        } else {
            s.probe.lock = Lock::Open;
        }
        s.answered = true;
        break;
    }
    case room::IdJoinSuccess:
    case room::IdJoinSuccessAsMod:
        s.joined = true; // (a room that does not look at the version: we are a member now, and leave at the next step)
        s.answered = true;
        break;
    case room::IdNameCollision:
    case room::IdMacCollision:
    case room::IdConsoleIdCollision:
        // (the password is looked at before these: the room has none, or ours was right)
        if (s.second_ask) {
            s.probe.secret_ok = true;
        } else {
            s.probe.lock = Lock::Open;
        }
        s.answered = true;
        break;
    default:
        break;
    }
}

bool ProbePass::Step(std::uint32_t wait_ms) {
    if (!host_) {
        for (Slot& s : slots_) {
            if (!s.done) {
                s.probe.state = State::Offline;
                s.done = true;
                s.changed = true;
            }
        }
        return false;
    }
    StartNext(azl::platform::NowMs());
    ENetEvent event;
    int served = enet_host_service(host_, &event, wait_ms);
    while (served > 0) {
        Slot* s = event.peer ? static_cast<Slot*>(event.peer->data) : nullptr;
        if (event.type == ENET_EVENT_TYPE_CONNECT) {
            if (s && s->peer == event.peer && !s->done) {
                OnConnect(*s, azl::platform::NowMs());
            } else {
                event.peer->data = nullptr;
                enet_peer_reset(event.peer); // (nobody we asked: a stranger, or a peer of a room that is over)
            }
        } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            if (s && !s->done) {
                OnPacket(*s, event.packet->data, event.packet->dataLength);
            }
            enet_packet_destroy(event.packet);
        } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            event.peer->data = nullptr; // (the peer may come back for another connection: it must not point at this room)
            if (s && s->peer == event.peer && !s->done) {
                s->peer = nullptr; // (ENet has let it go already)
                Finish(*s, s->connected_ms ? State::Online : State::Offline);
            }
        }
        served = enet_host_check_events(host_, &event);
    }
    const std::uint64_t now = azl::platform::NowMs();
    bool left = next_ < slots_.size();
    for (Slot& s : slots_) {
        if (s.done || !s.started_ms) {
            continue;
        }
        if (!s.connected_ms) {
            if (now - s.started_ms >= options_.connect_ms) {
                Finish(s, State::Offline);
            }
        } else if (s.joined || (s.probe.has_info && s.answered) || now - (s.second_ask ? s.asked_ms : s.connected_ms) >= options_.answer_ms) {
            Finish(s, State::Online);
        }
        left = left || !s.done;
    }
    return left;
}

std::vector<std::size_t> ProbePass::TakeChanged() {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].changed) {
            slots_[i].changed = false;
            out.push_back(i);
        }
    }
    return out;
}

} // namespace lobbies
