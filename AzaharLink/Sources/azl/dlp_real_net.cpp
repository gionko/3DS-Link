// The real Download Play on the UDS engine (azl/dlp_real_net.hpp).
#include "azl/dlp_real_net.hpp"

#include <cstring>

#include "azl/platform.hpp"

namespace azl {

// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)

namespace {

u32 Elapsed(u32 now, u32 stamp) {
    const s32 d = static_cast<s32>(now - stamp);
    return d < 0 ? 0u : static_cast<u32>(d);
}

}  // namespace

RealDlpNet::RealDlpNet(UdsEngine* engine) : engine_(engine) {
    engine_->SetLobbyBeaconKind(UdsEngine::LobbyBeaconKind::RealDlp);
}

u64 RealDlpNet::NowMs() {
    return platform::NowMs();
}

u64 RealDlpNet::NowTicks() {
    return platform::NowTicks();
}

u32 RealDlpNet::Now() const {
    return static_cast<u32>(platform::NowMs());
}

bool RealDlpNet::Acquire(const Identity& me) {
    if (held_) {
        return true;
    }
    if (engine_->InitializeForLobby(me.seed, me.name) != res::kSuccess) {
        return false;  // a game has the service
    }
    held_ = true;
    return true;
}

void RealDlpNet::Release() {
    if (held_) {
        held_ = false;
        engine_->Shutdown();  // (the network ends with its Deauthentications; the bind and its ring go with the service)
    }
    engine_->SetClientForeignFilterEnforced(false);  // (persistent in the engine, Shutdown keeps it: off on every path, held or not)
    ClearHost();
    ClearClient();
}

/// The probe and the session end (the scan and its cache stay: they are ScanClose's).
void RealDlpNet::ClearClient() {
    probe_ = Probe();
    session_ = false;
    session_flags_ = 0;
}

void RealDlpNet::ClearHost() {
    for (u32 s = 0; s < kMaxClients; ++s) {
        peers_[s] = Peer();
    }
    hosting_ = false;
    accepting_ = false;
    pass_set_ = false;
    grouped_ = false;
    group_now_ = false;
    spectators_ = 0;
    frame_ = dlp_real::kAnnounceFrames;
}

bool RealDlpNet::HostOpen(const HostAnnounce& a, u8 channel, const Identity& me) {
    if (a.max_nodes < 2 || a.max_nodes > kMaxPlayers) {
        return false;  // (a lobby of more than 4 players is never announced)
    }
    const bool took = !held_;
    if (!Acquire(me)) {
        return false;
    }
    if (engine_->BeginHostingDlp(a.max_nodes, channel) != res::kSuccess) {
        if (took) {
            Release();  // (only an engine this call took: a network already up is not ours to end because a second ask failed)
        }
        return false;
    }
    // (no handle is handed out: the bind is the adapter's, read by the Pump; Shutdown frees it)
    if (engine_->Bind(kHostBind, kDialogueRing, kChannelDialogue, kBroadcastNetworkNodeId, nullptr) != res::kSuccess) {
        Release();  // (the network this call began)
        return false;
    }
    ClearHost();
    hosting_ = true;
    child_tid_ = dlp_real::kChildTidHigh | a.child_tid_low;
    max_nodes_ = a.max_nodes;
    accepting_ = a.accepting != 0;
    if (!accepting_) {
        engine_->UpdateNetworkAttribute(kBlockClients, 1);  // (BeginHostingDlp leaves the admission open)
    }
    return true;
}

void RealDlpNet::HostUpdate(const HostAnnounce& a) {
    if (!hosting_) {
        return;
    }
    accepting_ = a.accepting != 0;  // (no more announcement once the host stops accepting: Azahar stops at StartDistribution)
    engine_->UpdateNetworkAttribute(kBlockClients, accepting_ ? 0 : 1);
}

void RealDlpNet::HostClose() {
    Release();
}

void RealDlpNet::Kick(u8 node) {
    if (!held_) {
        return;
    }
    engine_->EjectClient(node);  // (an id that is not there is no error; the host's own refuses)
    if (hosting_ && node >= kFirstClientNode && node <= kMaxPlayers) {
        Peer& p = peers_[node - kFirstClientNode];
        p = Peer();
        p.st = kEjected;  // (nothing more to it until the engine no longer lists the node)
    }
}

// ---- the searcher (design 5) ------------------------------------------------------------------------------------------

/// Takes nothing but the engine's table of beacons (the probes take the engine one at a time, in the Pump). Idempotent; a new scan forgets
/// what the last one learnt (Azahar empties its ignored list at StartScan).
bool RealDlpNet::ScanOpen() {
    if (hosting_ || session_ || (!held_ && engine_->initialized()) || !engine_->PrepareLobby()) {
        return false;  // (a game's session, or the adapter busy with something else)
    }
    if (!scanning_) {
        for (u32 i = 0; i < kProbeCache; ++i) {
            heard_[i] = Heard();
        }
        scanning_ = true;
        probe_looked_ = false;
    }
    return true;
}

/// A probe under way ends (the engine given back), the cache goes.
void RealDlpNet::ScanClose() {
    if (probe_.st != kProbeIdle) {
        Release();
    }
    scanning_ = false;
    for (u32 i = 0; i < kProbeCache; ++i) {
        heard_[i] = Heard();
    }
}

RealDlpNet::Heard* RealDlpNet::FindHeard(const Mac& host) {
    for (u32 i = 0; i < kProbeCache; ++i) {
        if (heard_[i].st != kHeardNone && heard_[i].host == host) {
            return &heard_[i];
        }
    }
    return nullptr;
}

u8 RealDlpNet::HeardState(const Mac& host) const {
    for (u32 i = 0; i < kProbeCache; ++i) {
        if (heard_[i].st != kHeardNone && heard_[i].host == host) {
            return heard_[i].st;
        }
    }
    return kHeardNone;
}

bool RealDlpNet::BeaconOf(const Mac& host, u32 max_age_ms, UdsEngine::HeardLobby* out) const {
    for (u32 i = 0; i < UdsEngine::kHeardLobbies; ++i) {
        if (!engine_->HeardLobbyAt(i, max_age_ms, out)) {
            return false;
        }
        if (out->host == host) {
            return true;
        }
    }
    return false;
}

/// The i-th verified host whose beacon is fresh (any game: the lobby filters). Not accepting when its network is full.
bool RealDlpNet::ScanRead(u32 index, HeardGroup* out) {
    UdsEngine::HeardLobby beacon;
    u32 listed = 0;
    for (u32 i = 0; i < UdsEngine::kHeardLobbies; ++i) {
        if (!engine_->HeardLobbyAt(i, kProbeFreshMs, &beacon)) {
            return false;
        }
        Heard* h = FindHeard(beacon.host);
        if (h == nullptr || h->st != kHeardVerified || listed++ != index) {
            continue;
        }
        h->t_ms = Now();  // (listed: the entry is in use)
        out->host = beacon.host;
        out->total_nodes = beacon.total_nodes;
        out->signal = 3;
        out->announce.child_tid_low = h->child_tid_low;
        out->announce.max_nodes = h->max_nodes;
        out->announce.accepting = beacon.total_nodes < beacon.max_nodes ? 1 : 0;
        for (u32 k = 0; k < kNameUnits; ++k) {
            out->announce.name[k] = h->name[k];
        }
        return true;
    }
    return false;
}

/// The join: the engine, the client's connection on the channel of the host's beacon (heard within the engine's 3 s: nothing of the scan
/// is needed), the session's bind, the filter on.
bool RealDlpNet::ConnectBegin(const Mac& host, const Identity& me) {
    if (hosting_ || session_) {
        return false;
    }
    if (probe_.st != kProbeIdle) {
        Release();  // (the lobby closed its scan first: a probe still under way is given up)
    }
    UdsEngine::HeardLobby beacon;
    if (!BeaconOf(host, kProbeConnectMs, &beacon)) {
        return false;  // (no such host now: the lobby answers "not found")
    }
    const bool took = !held_;
    if (!Acquire(me)) {
        return false;
    }
    if (engine_->ConnectBeginDlp(host, beacon.channel, ConnectionType::Client) != res::kSuccess) {
        if (took) {
            Release();
        }
        return false;
    }
    if (engine_->Bind(kSessionBind, kDialogueRing, kChannelDialogue, kHostNode, nullptr) != res::kSuccess) {
        Release();
        return false;
    }
    engine_->SetClientForeignFilterEnforced(true);  // (a second host of the Download Play in the room is node 1 too: design 5.2, Q8)
    session_ = true;
    session_flags_ = 0;
    return true;
}

int RealDlpNet::ConnectPoll() {
    return session_ ? engine_->ConnectPoll() : -1;
}

void RealDlpNet::Disconnect() {
    Release();
}

// ---- both -----------------------------------------------------------------------------------------------------------

u8 RealDlpNet::MyNode() {
    u16 node, mask;
    if (!engine_->NodeSnapshot(&node, &mask) || node > 0xFF) {
        return 0;
    }
    return static_cast<u8>(node);
}

/// A client that is no longer in the mask loses its dialogue: the next node with its id starts from the Auth. Done wherever the mask is read
/// (the Pump, and the lobby's own Members), so that the adapter forgets a client no later than the lobby does.
void RealDlpNet::ForgetGone(u16 mask) {
    for (u32 s = 0; s < kMaxClients; ++s) {
        if ((mask & (1u << (s + kFirstClientNode - 1))) == 0) {
            peers_[s] = Peer();
        }
    }
}

u32 RealDlpNet::Members(u8 (&nodes)[kMaxPlayers]) {
    u16 node = 0, mask = 0;
    const bool in = engine_->NodeSnapshot(&node, &mask);
    if (hosting_) {
        ForgetGone(mask);
    }
    if (!in) {
        return 0;
    }
    u32 count = 0;  // (the spectators are not in the mask: a searcher reading the announcement is never a client)
    for (u32 id = 1; id <= 16 && count < kMaxPlayers; ++id) {
        if ((mask & (1u << (id - 1))) != 0) {
            nodes[count++] = static_cast<u8>(id);
        }
    }
    return count;
}

bool RealDlpNet::ReadNode(u8 node, NodeEntry* out) {
    NodeInfo info;
    if (node == 0 || engine_->GetNodeInformation(node, &info) != res::kSuccess) {
        return false;  // (id 0 is "found, as zeros" in every state: it is no node)
    }
    out->seed = info.friend_code_seed;
    for (u32 i = 0; i < kNameUnits; ++i) {
        out->name[i] = info.username[i];
    }
    out->node = node;
    return true;
}

/// The lobby's START and GO to a client become wishes the Pump fulfils on the wire (no frame here: a game thread may be the caller).
/// Idempotent in every state: a START to a client already past the Auth changes nothing, the FIRST passphrase stays.
bool RealDlpNet::Send(u8 to_node, const u8* data, u32 size) {
    if (session_) {
        return to_node == kHostNode;  // (the searcher's START_ACK / GO_ACK: the Pump answered the host already)
    }
    if (!hosting_ || to_node < kFirstClientNode || to_node > kMaxPlayers || size < 2) {
        return false;
    }
    Peer& p = peers_[to_node - kFirstClientNode];
    if (data[0] == static_cast<u8>(MsgType::Start) && size == 2) {
        p.flags |= kWantStart;
        return true;
    }
    if (data[0] == static_cast<u8>(MsgType::Go) && size == kPassphraseOffset + kPassphraseSize) {
        if (!pass_set_) {
            for (u32 i = 0; i < kPassphraseSize; ++i) {
                pass_[i] = data[kPassphraseOffset + i];
            }
            pass_set_ = true;
        }
        p.flags |= kWantGo;
        return true;
    }
    return false;
}

/// The START_ACK / GO_ACK the dialogue earned, one per call, as the dialect's messages.
bool RealDlpNet::Receive(u8* from_node, u8* data, u32 capacity, u32* size) {
    if (session_) {
        // the searcher: the host's StartDistribution as a START, then its final as a GO with the passphrase, once each
        const u8 owed = (session_flags_ & kOweStart) != 0 ? kOweStart : (session_flags_ & kOweGo);
        if (owed == 0 || capacity < kMaxMessage) {
            return false;
        }
        session_flags_ = static_cast<u8>(session_flags_ & ~owed);
        *size = owed == kOweStart ? EncodeMessage(MsgType::Start, nullptr, 0, data)
                                  : EncodeMessage(MsgType::Go, pass_, kPassphraseSize, data);
        *from_node = static_cast<u8>(kHostNode);
        return *size != 0;
    }
    if (!hosting_ || capacity < 2) {
        return false;
    }
    for (u32 s = 0; s < kMaxClients; ++s) {
        Peer& p = peers_[s];
        const u8 owed = (p.flags & kOweStartAck) != 0 ? kOweStartAck : ((p.flags & kOweGoAck) != 0 ? kOweGoAck : 0);
        if (owed == 0) {
            continue;
        }
        p.flags = static_cast<u8>(p.flags & ~owed);
        data[0] = static_cast<u8>(owed == kOweStartAck ? MsgType::StartAck : MsgType::GoAck);
        data[1] = 0;
        *from_node = static_cast<u8>(s + kFirstClientNode);
        *size = 2;
        return true;
    }
    return false;
}

// <<< game-thread code

// ---- the network thread ---------------------------------------------------------------------------------------------

void RealDlpNet::Pump() {
    if (!hosting_ && !session_ && !scanning_) {
        return;
    }
    const u32 now = Now();
    if (pumped_ && Elapsed(now, pump_ms_) < kPumpMs) {
        return;
    }
    pump_ms_ = now;
    pumped_ = true;
    if (hosting_) {
        PumpHost(now);
    } else if (session_) {
        PumpSession();
    } else {
        PumpProbe(now);
    }
}

// ---- the searcher on the network thread ------------------------------------------------------------------------------

void RealDlpNet::PumpProbe(u32 now) {
    if (probe_.st == kProbeIdle) {
        if (!probe_looked_ || Elapsed(now, probe_ms_) >= kProbeEveryMs) {
            probe_looked_ = true;
            probe_ms_ = now;
            StartProbe(now);
        }
    } else if (probe_.st == kProbeConnecting) {
        const int r = engine_->ConnectPoll();
        if (r > 0) {
            if (engine_->Bind(kProbeBind, kProbeRing, kChannelAnnounce, kHostNode, nullptr) != res::kSuccess) {
                EndProbe(kHeardRetry, now);
                return;
            }
            probe_.st = kProbeCollect;
            probe_.t_ms = now;  // (Azahar's second clock starts at its bind too)
        } else if (r < 0 || Elapsed(now, probe_.t_ms) >= kProbeConnectMs) {
            EndProbe(kHeardRetry, now);  // (a host that does not answer is not ignored: tried again later)
        }
    } else {
        Collect(now);
    }
}

/// The first host with a fresh beacon that the cache does not know (or whose retry is due): the engine, a spectator's connection, the filter.
void RealDlpNet::StartProbe(u32 now) {
    UdsEngine::HeardLobby beacon;
    Heard* slot = nullptr;
    for (u32 i = 0; slot == nullptr; ++i) {
        if (i >= UdsEngine::kHeardLobbies || !engine_->HeardLobbyAt(i, kProbeFreshMs, &beacon)) {
            return;  // (nobody new)
        }
        if (beacon.total_nodes >= beacon.max_nodes) {
            continue;  // (a full host answers no spectator - Azahar's even asserts on a late one: not probed, not cached, looked at again)
        }
        slot = FindHeard(beacon.host);
        if (slot == nullptr) {  // a free entry, else the one used longest ago
            slot = &heard_[0];
            for (u32 k = 0; k < kProbeCache; ++k) {
                if (heard_[k].st == kHeardNone) {
                    slot = &heard_[k];
                    break;
                }
                if (Elapsed(now, heard_[k].t_ms) > Elapsed(now, slot->t_ms)) {
                    slot = &heard_[k];
                }
            }
        } else if (slot->st != kHeardRetry || Elapsed(now, slot->t_ms) < kProbeRetryMs) {
            slot = nullptr;  // (known: verified, ignored, or not due)
        }
    }
    if (!Acquire(me_)) {
        return;  // (a game has the service: no probe now)
    }
    if (engine_->ConnectBeginDlp(beacon.host, beacon.channel, ConnectionType::Spectator) != res::kSuccess) {
        Release();
        return;
    }
    engine_->SetClientForeignFilterEnforced(true);  // (two hosts in the room both send on channel 1 as node 1: only ours is read)
    *slot = Heard();
    slot->host = beacon.host;
    slot->st = kHeardRetry;  // (until the probe says otherwise: an engine taken away mid-probe leaves a host to try again)
    slot->t_ms = now;
    probe_ = Probe();
    probe_.host = beacon.host;
    probe_.st = kProbeConnecting;
    probe_.t_ms = now;
    Bump(&count_.probes);
}

/// At most kPullsPerProbe frames of the announcement. Every check of design 5.2: the length at +4, type 1, the index, the length of the index.
void RealDlpNet::Collect(u32 now) {
    for (u32 k = 0; k < kPullsPerProbe; ++k) {
        u32 n = 0;
        u16 src = 0;
        if (engine_->PullPacket(kProbeBind, kPullMax, buf_, sizeof(buf_), &n, &src) != res::kSuccess || n == 0) {
            break;
        }
        const dlp_real::Packet p = dlp_real::ParseAnnounce(buf_, n < sizeof(buf_) ? n : sizeof(buf_));
        if (p.kind != dlp_real::Kind::Announce) {
            EndProbe(kHeardIgnored, now);  // (as Azahar: a malformed frame ignores the host for the rest of the scan)
            return;
        }
        if (p.index == 0) {
            probe_.child_tid_low = static_cast<u32>(dlp_real::AnnounceChildTid(buf_));
            probe_.max_nodes = dlp_real::AnnounceMaxNodes(buf_);
        }
        probe_.mask = static_cast<u8>(probe_.mask | (1u << p.index));
        if (probe_.mask == (1u << dlp_real::kAnnounceFrames) - 1) {
            EndProbe(kHeardVerified, now);
            return;
        }
    }
    if (Elapsed(now, probe_.t_ms) >= kProbeCollectMs) {
        EndProbe(probe_.mask == 0 ? kHeardIgnored : kHeardNone, now);  // (nothing at all: ignored; a part: again at once)
    }
}

void RealDlpNet::EndProbe(u8 verdict, u32 now) {
    Heard* h = FindHeard(probe_.host);
    if (h != nullptr) {
        h->st = verdict;
        h->t_ms = now;
        if (verdict == kHeardVerified) {
            NodeInfo info;  // (the host's name: node 1's, which its Logoff gave)
            const bool named = engine_->GetNodeInformation(kHostNode, &info) == res::kSuccess;
            for (u32 k = 0; k < kNameUnits; ++k) {
                h->name[k] = named ? info.username[k] : 0;
            }
            h->child_tid_low = probe_.child_tid_low;
            h->max_nodes = probe_.max_nodes;
            Bump(&count_.verified);
        } else if (verdict == kHeardIgnored) {
            Bump(&count_.ignored_hosts);
        }
    }
    engine_->Unbind(kProbeBind);  // (here, on the network thread: no Release on a game thread frees its 8 KB ring later)
    engine_->DisconnectNetwork();
    Release();
}

// ---- the session on the network thread ------------------------------------------------------------------------------

/// At most kPullsPerPump packets from the host, each answered at once (design 5.4); the lobby hears StartDistribution and the final.
void RealDlpNet::PumpSession() {
    for (u32 k = 0; k < kPullsPerPump; ++k) {
        u32 n = 0;
        u16 src = 0;
        if (engine_->PullPacket(kSessionBind, kPullMax, buf_, sizeof(buf_), &n, &src) != res::kSuccess || n == 0) {
            return;
        }
        const dlp_real::Packet p = dlp_real::ParseFromHost(buf_, n < sizeof(buf_) ? n : sizeof(buf_));
        if (p.kind == dlp_real::Kind::None || src != kHostNode) {
            Bump(&count_.host_bad);
            continue;
        }
        if (p.kind == dlp_real::Kind::Final) {  // (not answered; the FIRST one's passphrase: an Azahar host sends one, ours up to 4)
            if ((session_flags_ & kSawFinal) == 0) {
                std::memcpy(pass_, buf_ + dlp_real::kPassphraseAt, kPassphraseSize);
                session_flags_ = static_cast<u8>(session_flags_ | kSawFinal | kOweGo);
            }
            continue;
        }
        if (p.kind == dlp_real::Kind::StartDist && (session_flags_ & kSawStart) == 0) {
            session_flags_ = static_cast<u8>(session_flags_ | kSawStart | kOweStart);
        }
        const std::size_t len = dlp_real::Reply(buf_, dlp_real::kTxSeed, p);
        engine_->SendTo(kHostNode, kChannelDialogue, static_cast<u32>(len), 0, buf_, len);
        Bump(&count_.answers);
    }
}

void RealDlpNet::PumpHost(u32 now) {
    u16 me = 0, mask = 0;
    engine_->NodeSnapshot(&me, &mask);  // (false = our network is gone: mask 0, every dialogue ends)
    ForgetGone(mask);
    TakePackets(now);
    for (u32 s = 0; s < kMaxClients; ++s) {
        const u32 node = s + kFirstClientNode;
        if ((mask & (1u << (node - 1))) != 0) {
            StepPeer(static_cast<u8>(node), now);
        }
    }
    Announce(now);
}

void RealDlpNet::TakePackets(u32 now) {
    for (u32 i = 0; i < kPullsPerPump; ++i) {
        u32 n = 0;
        u16 src = 0;
        if (engine_->PullPacket(kHostBind, kPullMax, buf_, sizeof(buf_), &n, &src) != res::kSuccess || n == 0) {
            return;
        }
        // (a frame longer than the buffer was taken out cut: its length field cannot match, the parser refuses it)
        const dlp_real::Packet p = dlp_real::ParseFromClient(buf_, n < sizeof(buf_) ? n : sizeof(buf_));
        if (p.kind == dlp_real::Kind::None || src < kFirstClientNode || src > kMaxPlayers) {
            Bump(&count_.bad);
            continue;
        }
        OnPacket(static_cast<u8>(src), p, now);
    }
}

void RealDlpNet::OnPacket(u8 node, const dlp_real::Packet& p, u32 now) {
    Peer& peer = peers_[node - kFirstClientNode];
    if (p.kind == dlp_real::Kind::AuthAck && peer.st == kAuthWait) {
        peer.rid[0] = p.rid[0];
        peer.rid[1] = p.rid[1];
        peer.st = kAuthed;
    } else if (p.kind == dlp_real::Kind::DistAck && peer.st == kDistWait) {
        if (p.needs_content) {  // (a download: this mode carries none; the lobby sees the node go and expels it)
            engine_->EjectClient(node);
            peer = Peer();
            peer.st = kEjected;
            Bump(&count_.content);
            return;
        }
        peer.st = kFinWait;
        peer.t_ms = now;
        SendDialogue(node, dlp_real::Finish(buf_, dlp_real::kTxSeed, 2, peer.rid));
        Bump(&count_.finish);
    } else if (p.kind == dlp_real::Kind::FinishAck && peer.st == kFinWait && !p.needs_content) {
        peer.st = kReady;
        peer.flags |= kOweStartAck;
    } else if (p.kind == dlp_real::Kind::NoticeAck && peer.st == kGoWait) {
        peer.st = kGoAcked;
        peer.flags |= kOweGoAck;
    } else if (p.kind == dlp_real::Kind::AuthAck || p.kind == dlp_real::Kind::DistAck || p.kind == dlp_real::Kind::FinishAck ||
               p.kind == dlp_real::Kind::NoticeAck) {
        Bump(&count_.ignored);  // (the answer to a packet resent, or one that came late: nothing to do)
    } else {
        Bump(&count_.bad);  // (a host's packet from a client)
    }
}

/// One step of a client's dialogue: at most one packet.
void RealDlpNet::StepPeer(u8 node, u32 now) {
    Peer& p = peers_[node - kFirstClientNode];
    switch (p.st) {
    case kFree:
        p.st = kAuthWait;
        p.t_ms = now;
        SendDialogue(node, dlp_real::Auth(buf_, dlp_real::kTxSeed));
        Bump(&count_.auth);
        break;
    case kAuthWait:
        if (Elapsed(now, p.t_ms) >= kAuthResendMs) {
            p.t_ms = now;
            SendDialogue(node, dlp_real::Auth(buf_, dlp_real::kTxSeed));
            Bump(&count_.auth);
        }
        break;
    case kAuthed:
        if ((p.flags & kWantStart) != 0) {
            p.st = kDistWait;
            p.t_ms = now;
            SendDialogue(node, dlp_real::StartDist(buf_, dlp_real::kTxSeed, 1, p.rid));
            Bump(&count_.dist);
        }
        break;
    case kDistWait:
        if (Elapsed(now, p.t_ms) >= kDistResendMs) {
            p.t_ms = now;
            SendDialogue(node, dlp_real::StartDist(buf_, dlp_real::kTxSeed, 1, p.rid));
            Bump(&count_.dist);
        }
        break;
    case kFinWait:
        if (Elapsed(now, p.t_ms) >= kFinishResendMs) {
            p.t_ms = now;
            SendDialogue(node, dlp_real::Finish(buf_, dlp_real::kTxSeed, 2, p.rid));
            Bump(&count_.finish);
        }
        break;
    case kReady:
        if ((p.flags & kWantGo) != 0 && pass_set_) {
            p.st = kGoWait;
            p.t_ms = now;
            p.t7_ms = now;
            p.flags = static_cast<u8>((p.flags & 0x0F) | (1u << kTriesShift));
            SendDialogue(node, dlp_real::Final(buf_, dlp_real::kTxSeed, 3, p.rid, pass_));
            Bump(&count_.final);
        }
        break;
    case kGoWait: {
        const u32 tries = static_cast<u32>(p.flags >> kTriesShift);
        if (tries < kFinalTries && Elapsed(now, p.t7_ms) >= kFinalResendMs) {
            p.t7_ms = now;
            p.t_ms = now;  // (the notice waits for the next round)
            p.flags = static_cast<u8>((p.flags & 0x0F) | ((tries + 1) << kTriesShift));
            SendDialogue(node, dlp_real::Final(buf_, dlp_real::kTxSeed, 3, p.rid, pass_));
            Bump(&count_.final);
        } else if (Elapsed(now, p.t_ms) >= kNoticeMs) {
            p.t_ms = now;
            SendDialogue(node, dlp_real::Notice(buf_, dlp_real::kTxSeed, 3, p.rid));
            Bump(&count_.notice);
        }
        break;
    }
    default:  // kGoAcked, kEjected: nothing more
        break;
    }
}

/// One frame of the announcement, while the host accepts and a spectator is connected: a group at once when a spectator comes,
/// then one every kAnnounceMs.
void RealDlpNet::Announce(u32 now) {
    const u32 specs = accepting_ ? engine_->SpectatorCount() : 0;
    if (specs == 0) {
        spectators_ = 0;
        frame_ = dlp_real::kAnnounceFrames;  // (a group under way stops: nobody reads it)
        group_now_ = false;
        return;
    }
    if (specs > spectators_) {
        group_now_ = true;  // (a new spectator binds after it is in: the group that starts now, or right after this one, is its)
    }
    spectators_ = specs;
    if (frame_ >= dlp_real::kAnnounceFrames) {
        if (!group_now_ && grouped_ && Elapsed(now, group_ms_) < kAnnounceMs) {
            return;
        }
        frame_ = 0;
        group_ms_ = now;
        grouped_ = true;
        group_now_ = false;
    }
    const std::size_t len = dlp_real::AnnounceFrame(buf_, frame_, child_tid_, max_nodes_, dlp_real::kTxSeed);
    ++frame_;
    engine_->SendTo(kBroadcastNetworkNodeId, kChannelAnnounce, static_cast<u32>(len), 0, buf_, len);
    Bump(&count_.announce);
}

void RealDlpNet::SendDialogue(u8 node, std::size_t len) {
    engine_->SendTo(node, kChannelDialogue, static_cast<u32>(len), 0, buf_, len);
}

}  // namespace azl
