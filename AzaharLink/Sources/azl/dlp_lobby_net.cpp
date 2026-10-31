// The fake Download Play lobby on the UDS engine (azl/dlp_lobby_net.hpp).
#include "azl/dlp_lobby_net.hpp"

#include "azl/platform.hpp"

namespace azl {

// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)

u64 EngineLobbyNet::NowMs() {
    return platform::NowMs();
}

u64 EngineLobbyNet::NowTicks() {
    return platform::NowTicks();
}

bool EngineLobbyNet::Acquire(const Identity& me) {
    if (held_) {
        return true;
    }
    if (engine_->InitializeForLobby(me.seed, me.name) != res::kSuccess) {
        return false; // a game has the service
    }
    held_ = true;
    return true;
}

void EngineLobbyNet::Release() {
    if (!held_) {
        return;
    }
    held_ = false;
    engine_->Shutdown(); // (it ends the network we are in, with its Deauthentications, and switches the lobby's inbox off)
}

bool EngineLobbyNet::HostOpen(const HostAnnounce& a, u8 channel, const Identity& me) {
    if (a.max_nodes > kMaxPlayers) {
        return false; // (a lobby of more than 4 players is never announced)
    }
    const bool took = !held_;
    if (!Acquire(me)) {
        return false;
    }
    u8 announce[kAnnounceSize];
    EncodeAnnounce(a, announce);
    if (engine_->BeginHostingLobby(UdsEngine::kLobbyWlanCommId, UdsEngine::kLobbyId8, channel, a.max_nodes, announce, kAnnounceSize) !=
        res::kSuccess) {
        if (took) {
            Release(); // (only an engine this call took: a lobby already up is not ours to end because a second ask failed)
        }
        return false;
    }
    if (a.accepting == 0) {
        engine_->UpdateNetworkAttribute(kBlockClients, 1); // (a lobby opened closed: BeginHostingLobby leaves the admission open)
    }
    return true;
}

void EngineLobbyNet::HostUpdate(const HostAnnounce& a) {
    if (!held_) {
        return;
    }
    u8 announce[kAnnounceSize];
    EncodeAnnounce(a, announce);
    engine_->SetApplicationData(announce, kAnnounceSize);
    engine_->UpdateNetworkAttribute(kBlockClients, a.accepting != 0 ? 0 : 1);
}

void EngineLobbyNet::HostClose() {
    Release();
}

void EngineLobbyNet::Kick(u8 node) {
    if (held_) {
        engine_->EjectClient(node); // (an id that is not there is no error; the host's own refuses)
    }
}

bool EngineLobbyNet::ScanOpen() {
    // Beacons are heard in any state, before Initialize included: nothing to open but the lobby's table. Only a game's session is in the way.
    if (held_) {
        return true;
    }
    return !engine_->initialized() && engine_->PrepareLobby();
}

void EngineLobbyNet::ScanClose() {
}

bool EngineLobbyNet::ScanRead(u32 index, HeardGroup* out) {
    UdsEngine::HeardLobby heard;
    u32 listed = 0;
    for (u32 i = 0; i < UdsEngine::kHeardLobbies; ++i) {
        if (!engine_->HeardLobbyAt(i, kHeardFreshMs, &heard)) {
            return false;
        }
        // (a beacon of the Download Play's ids that is no announcement of ours - an Azahar creator, a later version - is not a lobby)
        if (!DecodeAnnounce(heard.data, heard.size, &out->announce)) {
            continue;
        }
        if (listed++ == index) {
            out->host = heard.host;
            out->total_nodes = heard.total_nodes;
            out->signal = 3;
            return true;
        }
    }
    return false;
}

bool EngineLobbyNet::ConnectBegin(const Mac& host, const Identity& me) {
    const bool took = !held_;
    if (!Acquire(me)) {
        return false;
    }
    if (engine_->ConnectBeginLobby(host) != res::kSuccess) {
        if (took) {
            Release();
        }
        return false;
    }
    return true;
}

int EngineLobbyNet::ConnectPoll() {
    return engine_->ConnectPoll();
}

void EngineLobbyNet::Disconnect() {
    Release();
}

u8 EngineLobbyNet::MyNode() {
    u16 node, mask;
    if (!engine_->NodeSnapshot(&node, &mask) || node > 0xFF) {
        return 0;
    }
    return static_cast<u8>(node);
}

u32 EngineLobbyNet::Members(u8 (&nodes)[kMaxPlayers]) {
    u16 node, mask;
    if (!engine_->NodeSnapshot(&node, &mask)) {
        return 0;
    }
    u32 count = 0;
    for (u32 id = 1; id <= 16 && count < kMaxPlayers; ++id) {
        if ((mask & (1u << (id - 1))) != 0) {
            nodes[count++] = static_cast<u8>(id);
        }
    }
    return count;
}

bool EngineLobbyNet::ReadNode(u8 node, NodeEntry* out) {
    NodeInfo info;
    if (node == 0 || engine_->GetNodeInformation(node, &info) != res::kSuccess) {
        return false; // (id 0 is "found, as zeros" in every state: it is no node)
    }
    out->seed = info.friend_code_seed;
    for (u32 i = 0; i < kNameUnits; ++i) {
        out->name[i] = info.username[i];
    }
    out->node = node;
    return true;
}

bool EngineLobbyNet::Send(u8 to_node, const u8* data, u32 size) {
    if (to_node < 1 || to_node > kMaxPlayers || size == 0 || size > kMaxMessage) {
        return false;
    }
    return engine_->SendTo(to_node, UdsEngine::kLobbyChannel, size, 0, data, size) == res::kSuccess;
}

bool EngineLobbyNet::Receive(u8* from_node, u8* data, u32 capacity, u32* size) {
    return engine_->PopLobbyMessage(from_node, data, capacity, size);
}

// <<< game-thread code

} // namespace azl
