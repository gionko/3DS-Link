// FakeLobbyNet: an in-memory virtual network between N instances of DlpLobby, true to the ENGINE, not to the design
// (Tests/dlp_lobby_test.cpp, DESIGN-FASE1.md sez. 9.0). One FakeAir is the shared air and the simulated clock; each console
// is a FakeLobbyNet (a LobbyNet) with an index. One node = one instance.
//
// What it does like the engine does it:
//   * a client shows up in Members() of the host the moment the engine admits it, BEFORE any message;
//   * a host that is full or whose admissions are blocked (accepting = 0) does NOT refuse: ConnectPoll() stays 0 until
//     Disconnect(); nodes are given from the lowest free id (2..4);
//   * HostClose() / Kick() empty Members() of the client (Kick can be delayed with SetKickLag);
//   * ScanRead() may omit a lobby for a while (HideBeacon); ReadNode() returns the identity the node joined with;
//   * messages carry the engine's from_node and travel only inside their own lobby (two lobbies on one air never touch).
// What the bench can do (all deterministic): the clock (Set / Advance, also from 0xFFFFFF00, back and forward jumps);
// Drop / Dup / Delay / Reorder / Partition on messages by type, sender and receiver; FailSend, FailHostOpen,
// FailScanOpen, FailConnectBegin, ConnectDelay, ConnectResult(-1), ConnectPollCost; Phantom nodes, silent Leave, Flap
// (presence independent of messages); synthetic lobbies in the air (AddAir) for the scan tests; Inject. The knobs of a FAULTY
// engine: MyNodeZero (MyNode() says 0 while Members() still lists us), ForceMembers (Members() answers exactly this list, which
// may be longer than the array: the count is returned, at most four ids are written), FailReadNode.
// Counters: Sent(type, to), Kicked(node), HostCloseCalls / DisconnectCalls / ScanCloseCalls, ReadNodeCalls, the order of the
// closing calls (CloseOrder), the channel and the announcements of HostOpen / HostUpdate.
// It is NOT thread-safe: the test locks a mutex around every call into a lobby. It detects two threads inside it at the
// same time (overlaps()) and any call made while the test's lock flag is down (WatchLock / unlocked_calls()), which is how
// the test proves that the lock is held. Frames already delivered to a console stay in its ring until it disconnects, as
// in the adapter (a client keeps the GO that arrived just before the host closed the network).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "azl/dlp_lobby.hpp"

class FakeLobbyNet;

class FakeAir {
public:
    static constexpr int kPorts = 8;
    static constexpr int kNodes = 8;  // node ids 0..7 (0 unused; 5..7 only for phantoms)
    static constexpr int kAnyType = 0;
    static constexpr int kAny = 0xFF;
    static constexpr int kForever = -1;

    struct Port {
        azl::Mac mac = {};
        azl::Identity ident = {};
        int role = 0;    // 0 idle, 1 host, 2 scanning, 3 joining, 4 client
        int lobby = -1;  // the lobby (index of its host port) this port is in; -1 none
        azl::u8 node = 0;
        azl::u64 inbox_epoch = 0;  // the lobby incarnation whose frames this console still holds (its adapter's ring)
        azl::Mac connect_mac = {};
        azl::u64 connect_start = 0;
        bool connect_pending = false;
        int connect_forced = 0;  // -1 = ConnectPoll answers -1
        // knobs
        bool fail_host_open = false, fail_scan_open = false, fail_connect_begin = false;
        int fail_send = 0;
        azl::u64 connect_delay = 0, connect_poll_cost = 0;
        bool fail_read[kNodes] = {};
        bool my_node_zero = false;        // a faulty engine: MyNode() says 0 while Members() still lists our node
        bool members_forced = false;      // a faulty engine: Members() answers exactly `members_list`
        std::vector<azl::u8> members_list;
        // counters
        int host_open_calls = 0, host_update_calls = 0, host_close_calls = 0, scan_open_calls = 0, scan_close_calls = 0;
        int connect_begin_calls = 0, disconnect_calls = 0, members_calls = 0, scan_read_calls = 0, receive_calls = 0;
        int send_calls = 0, connect_poll_calls = 0, read_node_calls = 0, kicked[kNodes] = {}, sent[8][kNodes] = {};
        azl::u8 last_channel = 0xFF;
        azl::HostAnnounce last_announce = {};
        std::vector<azl::u8> accepting_log;  // the accepting flag of every HostOpen / HostUpdate, in order
        std::string order;                   // 'H' HostClose, 'D' Disconnect, 'S' ScanClose
        std::string last_msg[8];             // the bytes of the last message sent, by type (1..4)
    };

    explicit FakeAir(azl::u64 start_ms = 100000) : now_(start_ms) {}

    azl::u64 now() const { return now_; }
    void Set(azl::u64 ms) { now_ = ms; }
    void Advance(azl::u64 ms) { now_ += ms; }
    void SetTickOffset(azl::u64 off) { tick_off_ = off; }
    azl::u64 ticks() const { return now_ * 268112ull + tick_off_; }
    Port& port(int i) { return port_[i]; }
    int overlaps() const { return overlaps_.load(); }
    /// The lock test: calls into the network while `*flag` (set by the test while it holds its mutex) was false.
    void WatchLock(const std::atomic<bool>* flag) { lock_flag_ = flag; }
    int unlocked_calls() const { return unlocked_.load(); }

    // ---- bench commands -----------------------------------------------------------------------------------------
    void ClearRules() { rules_.clear(); }
    /// Drop the next `count` messages of `type` (0 = any) from node `from` to node `to` (kAny = any); kForever = for ever.
    void Drop(int type, int from, int to, int count) { AddRule(0, type, from, to, count, 0); }
    /// Deliver the next message of `type` 1 + n times.
    void Dup(int type, int n) { AddRule(1, type, kAny, kAny, 1, static_cast<azl::u64>(n)); }
    /// Hold every message of `type` for `ms` (until ClearRules).
    void Delay(int type, azl::u64 ms) { AddRule(2, type, kAny, kAny, kForever, ms); }
    /// Swap the next two messages of `type`.
    void Reorder(int type) { AddRule(3, type, kAny, kAny, 1, 0); }
    /// Messages from node `from` to node `to` are lost for `ms` from now.
    void Partition(int from, int to, azl::u64 ms) { AddRule(4, kAnyType, from, to, kForever, now_ + ms); }
    /// A node that is not a console: it shows in Members() of lobby `host` and nobody answers for it.
    void Phantom(int host, int node, azl::u64 seed, const char* name) {
        Slot& s = lobby_[host].slot[node];
        s = Slot();
        s.used = true;
        s.id.seed = seed;
        s.id.node = static_cast<azl::u8>(node);
        Name(s.id.name, name);
    }
    /// A node vanishes without a word (a console that fell off): gone from Members() at once.
    void Leave(int host, int node) { RemoveSlot(host, node); }
    /// A node is missing from Members() for `ms` and comes back (presence independent of messages).
    void Flap(int host, int node, azl::u64 ms) { lobby_[host].slot[node].hide_until = now_ + ms; }
    /// After Kick(node) the node lingers in Members() for `ms` more.
    void SetKickLag(azl::u64 ms) { kick_lag_ = ms; }
    /// The beacon of lobby `host` is not heard for `ms`.
    void HideBeacon(int host, azl::u64 ms) { lobby_[host].beacon_hide_until = now_ + ms; }
    /// The lobby of `host` disappears from the air at once, no word to anybody (clients lose their network).
    void Vanish(int host) { CloseLobby(host); }
    /// Puts a lobby that no console runs in the air (for the scan tests). Returns its index.
    int AddAir(const azl::Mac& host, const azl::HostAnnounce& a, azl::u8 total, azl::u8 signal = 3) {
        azl::HeardGroup g = {};
        g.host = host;
        g.announce = a;
        g.total_nodes = total;
        g.signal = signal;
        air_groups_.push_back(g);
        air_used_.push_back(true);
        return static_cast<int>(air_groups_.size()) - 1;
    }
    azl::HeardGroup& AirGroup(int i) { return air_groups_[static_cast<size_t>(i)]; }
    void RemoveAir(int i) { air_used_[static_cast<size_t>(i)] = false; }
    /// Puts a message straight into the inbox of console `to_port` as if the engine had delivered it from `from_node`.
    void Inject(int to_port, int from_node, const azl::u8* bytes, azl::u32 n) {
        Msg m = {};
        m.due = now_;
        m.epoch = port_[to_port].inbox_epoch;
        m.to_port = to_port;
        m.from = static_cast<azl::u8>(from_node);
        m.to = port_[to_port].node;
        m.size = static_cast<azl::u8>(n);
        std::memcpy(m.data, bytes, n);
        Push(m);
    }
    /// Number of nodes now present in the lobby run by console `host` (0 if it has none).
    int Present(int host) const {
        int n = 0;
        if (lobby_[host].open) {
            for (int i = 1; i < kNodes; ++i) {
                n += lobby_[host].slot[i].used ? 1 : 0;
            }
        }
        return n;
    }
    bool InLobby(int host, int node) const { return lobby_[host].open && lobby_[host].slot[node].used; }
    azl::u64 message_count() const { return msgs_.size(); }

    static void Name(azl::u16* dst, const char* s) {
        for (int i = 0; i < 10; ++i) {
            dst[i] = (s != nullptr && s[0] != 0) ? static_cast<azl::u16>(static_cast<unsigned char>(*s++)) : 0;
        }
    }

    // ---- what the consoles call (through FakeLobbyNet) -------------------------------------------------------------
    struct Guard {
        FakeAir& a;
        explicit Guard(FakeAir& x) : a(x) {
            if (a.depth_.fetch_add(1) != 0) {
                a.overlaps_.fetch_add(1);
            }
            if (a.lock_flag_ != nullptr && !a.lock_flag_->load()) {
                a.unlocked_.fetch_add(1);
            }
        }
        ~Guard() { a.depth_.fetch_sub(1); }
    };

    bool HostOpen(int p, const azl::HostAnnounce& a, azl::u8 channel, const azl::Identity& me) {
        Port& P = port_[p];
        ++P.host_open_calls;
        Settle();
        if (P.fail_host_open || P.role != 0) {
            return false;
        }
        Lobby& L = lobby_[p];
        L = Lobby();
        L.open = true;
        L.mac = P.mac;
        L.ann = a;
        L.channel = channel;
        L.epoch = next_epoch_++;
        Slot& s = L.slot[1];
        s.used = true;
        s.port = p;
        s.id.seed = me.seed;
        s.id.node = 1;
        std::memcpy(s.id.name, me.name, sizeof(s.id.name));
        P.role = 1;
        P.lobby = p;
        P.node = 1;
        P.inbox_epoch = L.epoch;
        P.ident = me;
        P.last_channel = channel;
        P.last_announce = a;
        P.accepting_log.push_back(a.accepting);
        return true;
    }
    void HostUpdate(int p, const azl::HostAnnounce& a) {
        Port& P = port_[p];
        ++P.host_update_calls;
        Settle();
        if (P.role == 1) {
            lobby_[p].ann = a;
        }
        P.last_announce = a;
        P.accepting_log.push_back(a.accepting);
    }
    void HostClose(int p) {
        Port& P = port_[p];
        ++P.host_close_calls;
        P.order += 'H';
        Settle();
        if (P.role == 1) {
            CloseLobby(p);
        }
    }
    void Kick(int p, azl::u8 node) {
        Port& P = port_[p];
        if (node < kNodes) {
            ++P.kicked[node];
        }
        Settle();
        if (P.role == 1 && node >= 1 && node < kNodes && lobby_[p].slot[node].used && node != 1) {
            Slot& s = lobby_[p].slot[node];
            if (kick_lag_ == 0) {
                RemoveSlot(p, node);
            } else if (!s.removing) {
                s.removing = true;
                s.remove_at = now_ + kick_lag_;
            }
        }
    }
    bool ScanOpen(int p) {
        Port& P = port_[p];
        ++P.scan_open_calls;
        Settle();
        if (P.fail_scan_open || (P.role != 0 && P.role != 2)) {
            return false;
        }
        P.role = 2;
        return true;
    }
    void ScanClose(int p) {
        Port& P = port_[p];
        ++P.scan_close_calls;
        P.order += 'S';
        if (P.role == 2) {
            P.role = 0;
        }
    }
    bool ScanRead(int p, azl::u32 index, azl::HeardGroup* out) {
        Port& P = port_[p];
        ++P.scan_read_calls;
        Settle();
        if (P.role != 2) {
            return false;
        }
        azl::u32 seen = 0;
        for (int h = 0; h < kPorts; ++h) {
            const Lobby& L = lobby_[h];
            if (h == p || !L.open || now_ < L.beacon_hide_until) {
                continue;
            }
            if (seen++ == index) {
                azl::HeardGroup g = {};
                g.host = L.mac;
                g.announce = L.ann;
                g.total_nodes = static_cast<azl::u8>(VisibleCount(h));
                g.signal = 3;
                *out = g;
                return true;
            }
        }
        for (size_t i = 0; i < air_groups_.size(); ++i) {
            if (air_used_[i] && seen++ == index) {
                *out = air_groups_[i];
                return true;
            }
        }
        return false;
    }
    bool ConnectBegin(int p, const azl::Mac& host) {
        Port& P = port_[p];
        ++P.connect_begin_calls;
        Settle();
        if (P.fail_connect_begin || P.role != 0) {
            return false;
        }
        P.role = 3;
        P.inbox_epoch = 0;
        P.connect_mac = host;
        P.connect_start = now_;
        P.connect_pending = true;
        return true;
    }
    int ConnectPoll(int p) {
        Port& P = port_[p];
        ++P.connect_poll_calls;
        now_ += P.connect_poll_cost;  // a slow engine call
        Settle();
        if (P.role == 4) {
            return 1;
        }
        if (P.role == 3 && P.connect_forced < 0) {
            return -1;
        }
        return 0;
    }
    void Disconnect(int p) {
        Port& P = port_[p];
        ++P.disconnect_calls;
        P.order += 'D';
        Settle();
        if (P.role == 4) {
            RemoveSlot(P.lobby, P.node);
        }
        P.inbox_epoch = 0;  // the adapter drops its ring
        if (P.role == 3 || P.role == 4) {
            P.role = 0;
            P.lobby = -1;
            P.node = 0;
            P.connect_pending = false;
        }
    }
    azl::u8 MyNode(int p) {
        Settle();
        const Port& P = port_[p];
        return (!P.my_node_zero && (P.role == 1 || P.role == 4)) ? P.node : 0;
    }
    azl::u32 Members(int p, azl::u8 (&nodes)[azl::kMaxPlayers]) {
        Port& P = port_[p];
        ++P.members_calls;
        Settle();
        if (P.members_forced) {  // a faulty engine: the count may exceed what fits in the array (only four ids are written)
            for (size_t i = 0; i < P.members_list.size() && i < azl::kMaxPlayers; ++i) {
                nodes[i] = P.members_list[i];
            }
            return static_cast<azl::u32>(P.members_list.size());
        }
        azl::u32 n = 0;
        if ((P.role == 1 || P.role == 4) && lobby_[P.lobby].open) {
            for (int i = 1; i < kNodes && n < azl::kMaxPlayers; ++i) {
                const Slot& s = lobby_[P.lobby].slot[i];
                if (s.used && now_ >= s.hide_until) {
                    nodes[n++] = static_cast<azl::u8>(i);
                }
            }
        }
        return n;
    }
    bool ReadNode(int p, azl::u8 node, azl::NodeEntry* out) {
        Port& P = port_[p];
        ++P.read_node_calls;
        Settle();
        if ((P.role != 1 && P.role != 4) || node >= kNodes || P.fail_read[node] || !lobby_[P.lobby].open ||
            !lobby_[P.lobby].slot[node].used) {
            return false;
        }
        *out = lobby_[P.lobby].slot[node].id;
        return true;
    }
    bool Send(int p, azl::u8 to, const azl::u8* data, azl::u32 size) {
        Port& P = port_[p];
        ++P.send_calls;
        Settle();
        if (to < 1 || to > 4 || data == nullptr || size == 0 || size > azl::kMaxMessage) {
            return false;
        }
        if ((P.role != 1 && P.role != 4) || !lobby_[P.lobby].open || !lobby_[P.lobby].slot[to].used) {
            return false;
        }
        if (P.fail_send > 0) {
            --P.fail_send;
            return false;
        }
        ++P.sent[data[0] < 8 ? data[0] : 0][to];
        P.last_msg[data[0] < 8 ? data[0] : 0].assign(reinterpret_cast<const char*>(data), size);
        Msg m = {};
        m.due = now_;
        m.epoch = lobby_[P.lobby].epoch;
        m.to_port = lobby_[P.lobby].slot[to].port;
        m.from = P.node;
        m.to = to;
        m.size = static_cast<azl::u8>(size);
        std::memcpy(m.data, data, size);
        Enqueue(m);
        return true;
    }
    bool Receive(int p, azl::u8* from, azl::u8* data, azl::u32 capacity, azl::u32* size) {
        Port& P = port_[p];
        ++P.receive_calls;
        Settle();
        int best = -1;
        for (size_t i = 0; i < msgs_.size();) {
            const Msg& m = msgs_[i];
            if (m.to_port != p) {
                ++i;
                continue;
            }
            if (m.epoch != P.inbox_epoch) {  // another lobby than ours: the frame is lost
                msgs_.erase(msgs_.begin() + static_cast<long>(i));
                if (best >= static_cast<int>(i)) {
                    --best;
                }
                continue;
            }
            if (m.due <= now_ && (best < 0 || Before(m, msgs_[static_cast<size_t>(best)]))) {
                best = static_cast<int>(i);
            }
            ++i;
        }
        if (best < 0) {
            return false;
        }
        const Msg m = msgs_[static_cast<size_t>(best)];
        msgs_.erase(msgs_.begin() + best);
        const azl::u32 n = m.size < capacity ? m.size : capacity;
        std::memcpy(data, m.data, n);
        *from = m.from;
        *size = n;
        return true;
    }

private:
    friend class FakeLobbyNet;
    struct Slot {
        bool used = false;
        int port = -1;
        azl::NodeEntry id = {};
        azl::u64 hide_until = 0;
        bool removing = false;
        azl::u64 remove_at = 0;
    };
    struct Lobby {
        bool open = false;
        azl::Mac mac = {};
        azl::HostAnnounce ann = {};
        azl::u8 channel = 0;
        azl::u64 epoch = 0;
        Slot slot[kNodes];
        azl::u64 beacon_hide_until = 0;
    };
    struct Msg {
        azl::u64 due;
        azl::u64 seq;
        azl::u64 epoch;
        int to_port;
        azl::u8 from;
        azl::u8 to;
        azl::u8 size;
        azl::u8 data[azl::kMaxMessage];
    };
    struct Rule {
        int kind;  // 0 drop, 1 dup, 2 delay, 3 reorder, 4 partition
        int type;
        int from;
        int to;
        int count;
        azl::u64 param;
        bool holding;
        Msg held;
    };

    static bool Before(const Msg& a, const Msg& b) { return a.due < b.due || (a.due == b.due && a.seq < b.seq); }
    void AddRule(int kind, int type, int from, int to, int count, azl::u64 param) {
        Rule r = {};
        r.kind = kind;
        r.type = type;
        r.from = from;
        r.to = to;
        r.count = count;
        r.param = param;
        rules_.push_back(r);
    }
    int VisibleCount(int host) const {
        int n = 0;
        for (int i = 1; i < kNodes; ++i) {
            n += (lobby_[host].slot[i].used && now_ >= lobby_[host].slot[i].hide_until) ? 1 : 0;
        }
        return n;
    }
    void RemoveSlot(int host, int node) {
        if (host < 0 || node < 1 || node >= kNodes) {
            return;
        }
        Slot& s = lobby_[host].slot[node];
        if (s.port >= 0 && s.port != host) {
            Port& c = port_[s.port];
            if (c.role == 4 && c.lobby == host) {
                c.role = 0;
                c.lobby = -1;
                c.node = 0;
            }
        }
        s = Slot();
    }
    void CloseLobby(int host) {
        Lobby& L = lobby_[host];
        for (int i = 2; i < kNodes; ++i) {
            if (L.slot[i].used) {
                RemoveSlot(host, i);
            }
        }
        L = Lobby();
        Port& H = port_[host];
        if (H.role == 1) {
            H.role = 0;
            H.lobby = -1;
            H.node = 0;
            H.inbox_epoch = 0;
        }
    }
    bool TryAdmit(int p) {
        Port& P = port_[p];
        for (int h = 0; h < kPorts; ++h) {
            Lobby& L = lobby_[h];
            if (!L.open || !(L.mac == P.connect_mac)) {
                continue;
            }
            int used = 0;
            for (int i = 1; i < kNodes; ++i) {
                used += L.slot[i].used ? 1 : 0;
            }
            if (!L.ann.accepting || used >= L.ann.max_nodes) {
                return false;  // the engine stays silent
            }
            for (int node = 2; node <= 4; ++node) {
                if (!L.slot[node].used) {
                    Slot& s = L.slot[node];
                    s = Slot();
                    s.used = true;
                    s.port = p;
                    s.id.seed = P.ident.seed;
                    s.id.node = static_cast<azl::u8>(node);
                    std::memcpy(s.id.name, P.ident.name, sizeof(s.id.name));
                    P.role = 4;
                    P.lobby = h;
                    P.node = static_cast<azl::u8>(node);
                    P.inbox_epoch = L.epoch;
                    P.connect_pending = false;
                    return true;
                }
            }
            return false;
        }
        return false;
    }
    /// What the engine does by itself as time passes: delayed removals, admissions.
    void Settle() {
        for (int h = 0; h < kPorts; ++h) {
            if (!lobby_[h].open) {
                continue;
            }
            for (int n = 1; n < kNodes; ++n) {
                const Slot& s = lobby_[h].slot[n];
                if (s.used && s.removing && s.remove_at <= now_) {
                    RemoveSlot(h, n);
                }
            }
        }
        for (int p = 0; p < kPorts; ++p) {
            Port& P = port_[p];
            if (P.role == 3 && P.connect_pending && P.connect_forced == 0 && now_ >= P.connect_start + P.connect_delay) {
                TryAdmit(p);
            }
        }
    }
    void Push(Msg m) {
        m.seq = next_seq_++;
        msgs_.push_back(m);
    }
    static bool Match(const Rule& r, const Msg& m) {
        return (r.type == kAnyType || r.type == m.data[0]) && (r.from == kAny || r.from == m.from) &&
               (r.to == kAny || r.to == m.to);
    }
    void Enqueue(Msg m) {
        int copies = 1;
        for (Rule& r : rules_) {
            if (!Match(r, m)) {
                continue;
            }
            if (r.kind == 0 && r.count != 0) {
                if (r.count > 0) {
                    --r.count;
                }
                return;
            }
            if (r.kind == 4 && now_ < r.param) {
                return;
            }
            if (r.kind == 1 && r.count != 0) {
                --r.count;
                copies += static_cast<int>(r.param);
            }
            if (r.kind == 2 && r.count != 0) {
                m.due = now_ + r.param;
            }
            if (r.kind == 3 && r.count != 0) {
                if (!r.holding) {
                    r.held = m;
                    r.holding = true;
                    return;
                }
                r.holding = false;
                --r.count;
                for (int i = 0; i < copies; ++i) {
                    Push(m);
                }
                Push(r.held);
                return;
            }
        }
        if (m.to_port < 0) {
            return;  // a phantom node does not answer, and does not read
        }
        for (int i = 0; i < copies; ++i) {
            Push(m);
        }
    }

    azl::u64 now_;
    azl::u64 tick_off_ = 0;
    azl::u64 kick_lag_ = 0;
    azl::u64 next_seq_ = 1;
    azl::u64 next_epoch_ = 1;
    Port port_[kPorts];
    Lobby lobby_[kPorts];
    std::vector<Msg> msgs_;
    std::vector<Rule> rules_;
    std::vector<azl::HeardGroup> air_groups_;
    std::vector<bool> air_used_;
    std::atomic<int> depth_{0};
    std::atomic<int> overlaps_{0};
    std::atomic<int> unlocked_{0};
    const std::atomic<bool>* lock_flag_ = nullptr;
};

class FakeLobbyNet : public azl::LobbyNet {
public:
    /// Console number `index` on `air`, with its MAC (the radio address its nwm::UDS would use).
    FakeLobbyNet(FakeAir& air, int index) : air_(air), i_(index) {
        FakeAir::Port& P = air.port(index);
        P.mac = {0x40, 0xF4, 0x07, 0x11, 0x22, static_cast<azl::u8>(0x10 + index)};
    }
    int index() const { return i_; }
    FakeAir& air() { return air_; }
    FakeAir::Port& st() { return air_.port(i_); }
    const azl::Mac& mac() { return st().mac; }

    // LobbyNet
    azl::u64 NowMs() override { FakeAir::Guard g(air_); return air_.now(); }
    azl::u64 NowTicks() override { FakeAir::Guard g(air_); return air_.ticks(); }
    bool HostOpen(const azl::HostAnnounce& a, azl::u8 channel, const azl::Identity& me) override {
        FakeAir::Guard g(air_);
        return air_.HostOpen(i_, a, channel, me);
    }
    void HostUpdate(const azl::HostAnnounce& a) override { FakeAir::Guard g(air_); air_.HostUpdate(i_, a); }
    void HostClose() override { FakeAir::Guard g(air_); air_.HostClose(i_); }
    void Kick(azl::u8 node) override { FakeAir::Guard g(air_); air_.Kick(i_, node); }
    bool ScanOpen() override { FakeAir::Guard g(air_); return air_.ScanOpen(i_); }
    void ScanClose() override { FakeAir::Guard g(air_); air_.ScanClose(i_); }
    bool ScanRead(azl::u32 index, azl::HeardGroup* out) override {
        FakeAir::Guard g(air_);
        return air_.ScanRead(i_, index, out);
    }
    bool ConnectBegin(const azl::Mac& host, const azl::Identity& me) override {
        FakeAir::Guard g(air_);
        st().ident = me;
        return air_.ConnectBegin(i_, host);
    }
    int ConnectPoll() override { FakeAir::Guard g(air_); return air_.ConnectPoll(i_); }
    void Disconnect() override { FakeAir::Guard g(air_); air_.Disconnect(i_); }
    azl::u8 MyNode() override { FakeAir::Guard g(air_); return air_.MyNode(i_); }
    azl::u32 Members(azl::u8 (&nodes)[azl::kMaxPlayers]) override {
        FakeAir::Guard g(air_);
        return air_.Members(i_, nodes);
    }
    bool ReadNode(azl::u8 node, azl::NodeEntry* out) override {
        FakeAir::Guard g(air_);
        return air_.ReadNode(i_, node, out);
    }
    bool Send(azl::u8 to_node, const azl::u8* data, azl::u32 size) override {
        FakeAir::Guard g(air_);
        return air_.Send(i_, to_node, data, size);
    }
    bool Receive(azl::u8* from_node, azl::u8* data, azl::u32 capacity, azl::u32* size) override {
        FakeAir::Guard g(air_);
        return air_.Receive(i_, from_node, data, capacity, size);
    }

    // knobs and counters of this console
    void FailSend(int n) { st().fail_send = n; }
    void FailHostOpen(bool on) { st().fail_host_open = on; }
    void FailScanOpen(bool on) { st().fail_scan_open = on; }
    void FailConnectBegin(bool on) { st().fail_connect_begin = on; }
    void ConnectDelay(azl::u64 ms) { st().connect_delay = ms; }
    void ConnectResult(int r) { st().connect_forced = r; }
    void ConnectPollCost(azl::u64 ms) { st().connect_poll_cost = ms; }
    void FailReadNode(int node, bool on) { st().fail_read[node] = on; }
    /// A faulty engine: MyNode() answers 0 while Members() still lists us.
    void MyNodeZero(bool on) { st().my_node_zero = on; }
    /// A faulty engine: Members() answers exactly these ids (the count may be above four: four are written).
    void ForceMembers(std::initializer_list<int> ids) {
        st().members_forced = true;
        st().members_list.clear();
        for (int i : ids) {
            st().members_list.push_back(static_cast<azl::u8>(i));
        }
    }
    void UnforceMembers() { st().members_forced = false; }
    int ReadNodeCalls() { return st().read_node_calls; }
    int Sent(int type, int to) { return st().sent[type][to]; }
    int SentTotal(int type) {
        int n = 0;
        for (int t = 0; t < FakeAir::kNodes; ++t) {
            n += st().sent[type][t];
        }
        return n;
    }
    int Kicked(int node) { return st().kicked[node]; }
    /// The bytes of the last message of `type` this console sent (a string of the raw bytes).
    const std::string& LastMsg(int type) { return st().last_msg[type]; }
    int HostOpenCalls() { return st().host_open_calls; }
    int HostUpdateCalls() { return st().host_update_calls; }
    int HostCloseCalls() { return st().host_close_calls; }
    int DisconnectCalls() { return st().disconnect_calls; }
    int ScanOpenCalls() { return st().scan_open_calls; }
    int ScanCloseCalls() { return st().scan_close_calls; }
    int ConnectBeginCalls() { return st().connect_begin_calls; }
    int MembersCalls() { return st().members_calls; }
    int ScanReadCalls() { return st().scan_read_calls; }
    int ReceiveCalls() { return st().receive_calls; }
    int SendCalls() { return st().send_calls; }
    int ConnectPollCalls() { return st().connect_poll_calls; }
    /// Every call into the network of this console (the lock test: none outside the mutex).
    int AllCalls() {
        const FakeAir::Port& P = st();
        return P.host_open_calls + P.host_update_calls + P.host_close_calls + P.scan_open_calls + P.scan_close_calls +
               P.connect_begin_calls + P.disconnect_calls + P.members_calls + P.scan_read_calls + P.receive_calls +
               P.send_calls + P.connect_poll_calls;
    }
    int LastChannel() { return st().last_channel; }
    const azl::HostAnnounce& LastAnnounce() { return st().last_announce; }
    int Announces() { return static_cast<int>(st().accepting_log.size()); }
    const std::vector<azl::u8>& AcceptingLog() { return st().accepting_log; }
    const std::string& CloseOrder() { return st().order; }

private:
    FakeAir& air_;
    int i_;
};
