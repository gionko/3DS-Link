// Azahar-peer rule checker (spec C11): a test oracle that looks at every WifiPacket one of OUR
// engines puts on the wire and fails on the things that make a real Azahar peer ASSERT or misparse.
// Azahar's own frame code is trusted by Tests/azahar_ref (byte for byte); this checks the protocol
// *behaviour* around it, which that test cannot see. The rules (nwm_uds.cpp of Azahar):
//
//   secure-management  no SecureData with the management bit      (:394 ASSERT on receipt)
//   assoc-status       AssociationResponse status is always 0       (:177 ASSERT in the joining client)
//   start-to-full      no EAPoL-Start to a host that is full        (:224 ASSERT on the host)
//   logoff-broadcast   a broadcast EAPoL-Logoff only answers the FIRST Start of a client; a Connecting
//                      client adopts ANY Logoff it sees as its own admission, so a repeat must be
//                      unicast to that client (or not sent at all)
//   deauth-unknown     a Deauthentication goes to the broadcast MAC or to a MAC that is a room member
//                      or has been heard from; never to a guessed one (e.g. zeros)
//   pre-deauth-unscanned  a Deauthentication sent right before an Authentication to the same MAC (the
//                      client's "start over") needs a host whose beacon was seen (or that it spoke to
//                      before, e.g. when it left): never a guessed MAC
//   secure-shape       SecureData as Azahar takes it: aggregate count 0, one sub-frame whose sizes agree (the
//                      sequence number is free: the plugin numbers its frames 1, 2, 3 ... as the real nwm does,
//                      Azahar emits 0 and reads nothing of the field)
//
// Use: every test transport calls g_rules.Inspect(packet) once the transmitter address is filled in
// (see Console in mock_room.hpp), registers the room members with AddMember(), and the scenario ends
// with CHECK_RULES(). Thread safe: engines emit from the room thread, the game thread and the net
// thread.
#pragma once

#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "azl/uds_frames.hpp"
#include "support.hpp"

class AzaharRules {
public:
    struct Violation {
        std::string rule;
        std::string detail;
    };

    /// Forgets everything (call between independent scenarios).
    void Reset() {
        std::lock_guard<std::mutex> l(m_);
        violations_.clear();
        known_.clear();
        beaconed_.clear();
        talked_.clear();
        last_deauth_.clear();
        hosts_.clear();
        inspected_ = 0;
    }

    /// A MAC that is a member of the room (a Deauthentication to it is legitimate).
    void AddMember(const Mac& mac) {
        std::lock_guard<std::mutex> l(m_);
        known_.insert(mac);
    }

    /// Reports a violation found by something else (the Azahar peer model).
    void Fail(const char* rule, const std::string& detail) {
        std::lock_guard<std::mutex> l(m_);
        violations_.push_back({rule, detail});
    }

    /// `p` is a packet one of our engines is sending; its transmitter address is filled in.
    void Inspect(const WifiPacket& p) {
        std::lock_guard<std::mutex> l(m_);
        ++inspected_;
        switch (p.type) {
        case WifiPacket::Type::AssociationResponse: {
            u16 status = 0, aid = 0;
            if (!GetAssociationResult(p.data, &status, &aid)) {
                Add("assoc-status", "unparsable AssociationResponse from " + Name(p.transmitter_address));
            } else if (status != 0) {
                Add("assoc-status", "AssociationResponse with status " + std::to_string(status) + " from " +
                                        Name(p.transmitter_address));
            }
            break;
        }
        case WifiPacket::Type::Deauthentication:
            InspectDeauth(p);
            break;
        case WifiPacket::Type::Authentication:
            InspectAuth(p);
            break;
        case WifiPacket::Type::Beacon:
            beaconed_.insert(p.transmitter_address);
            break;
        case WifiPacket::Type::Data:
            InspectData(p);
            break;
        default:
            break;
        }
        if (p.type != WifiPacket::Type::Deauthentication) {
            last_deauth_.erase(p.transmitter_address); // (only the frame right before an Authentication counts)
            talked_.insert({p.transmitter_address, p.destination_address});
        }
        known_.insert(p.transmitter_address);
    }

    std::size_t count() const {
        std::lock_guard<std::mutex> l(m_);
        return violations_.size();
    }
    std::size_t inspected() const {
        std::lock_guard<std::mutex> l(m_);
        return inspected_;
    }
    std::vector<Violation> violations() const {
        std::lock_guard<std::mutex> l(m_);
        return violations_;
    }
    /// True if a violation of `rule` was recorded.
    bool Has(const char* rule) const {
        std::lock_guard<std::mutex> l(m_);
        for (const Violation& v : violations_) {
            if (v.rule == rule) {
                return true;
            }
        }
        return false;
    }
    std::string Report() const {
        std::lock_guard<std::mutex> l(m_);
        std::string out;
        for (const Violation& v : violations_) {
            out += "    " + v.rule + ": " + v.detail + "\n";
        }
        return out;
    }

private:
    struct Host {
        u8 max_nodes = 0;
        u8 total_nodes = 0;
        std::set<Mac> admitted;  // clients that got their Logoff
        std::deque<Mac> pending; // first Starts not answered yet
    };

    static std::string Name(const Mac& m) {
        return Hex(m.data(), 6);
    }
    void Add(const char* rule, const std::string& detail) {
        violations_.push_back({rule, detail});
    }

    void InspectAuth(const WifiPacket& p) {
        AuthSeq seq;
        if (!GetAuthenticationSeqNumber(p.data, &seq) || seq != AuthSeq::Seq1) {
            return;
        }
        const Mac& from = p.transmitter_address;
        const Mac& to = p.destination_address;
        auto deauth = last_deauth_.find(from);
        if (deauth != last_deauth_.end() && deauth->second == to && beaconed_.count(to) == 0 &&
            talked_.count({from, to}) == 0) {
            Add("pre-deauth-unscanned", Name(from) + " sends a Deauthentication and an Authentication to " + Name(to) +
                                            ", whose beacon it never saw and which it never spoke to");
        }
        // A client that authenticates again starts a new association: its first Start afterwards is a
        // first admission for the host that replaced the old one (one that ignores the Authentication,
        // like Azahar, never answers that Start, so nothing is lost by forgetting it here).
        auto host = hosts_.find(to);
        if (host != hosts_.end() && host->second.admitted.erase(from) && host->second.total_nodes > 1) {
            --host->second.total_nodes;
        }
    }

    void InspectDeauth(const WifiPacket& p) {
        const Mac& from = p.transmitter_address;
        const Mac& to = p.destination_address;
        last_deauth_[from] = to;
        if (to != kBroadcastMac && known_.count(to) == 0) {
            Add("deauth-unknown", Name(from) + " sends a Deauthentication to " + Name(to) +
                                      ", which is neither a room member nor heard from");
        }
        // Bookkeeping for the other rules: a client that leaves (or is thrown out) is a stranger again.
        auto host = hosts_.find(to);
        if (host != hosts_.end() && host->second.admitted.erase(from)) { // client -> host
            if (host->second.total_nodes > 1) {
                --host->second.total_nodes;
            }
        }
        host = hosts_.find(from);
        if (host != hosts_.end()) { // host -> client(s)
            if (to == kBroadcastMac) {
                host->second.admitted.clear();
                host->second.pending.clear();
                host->second.total_nodes = 1;
            } else if (host->second.admitted.erase(to) && host->second.total_nodes > 1) {
                --host->second.total_nodes;
            }
        }
    }

    void InspectData(const WifiPacket& p) {
        EtherType type;
        if (!GetFrameEtherType(p.data, &type)) {
            return;
        }
        if (type == EtherType::SecureData) {
            InspectSecure(p);
            return;
        }
        u16 magic = 0;
        if (!GetEapolFrameType(p.data, &magic)) {
            return;
        }
        if (magic == kEapolStartMagic) {
            Host& host = hosts_[p.destination_address];
            // (Azahar looks at an admitted client's repeated Start before it asserts on the count)
            if (host.max_nodes != 0 && host.total_nodes >= host.max_nodes &&
                host.admitted.count(p.transmitter_address) == 0) {
                Add("start-to-full", Name(p.transmitter_address) + " sends an EAPoL-Start to host " +
                                         Name(p.destination_address) + ", which already has " +
                                         std::to_string(host.total_nodes) + "/" + std::to_string(host.max_nodes) +
                                         " nodes");
            }
            if (host.admitted.count(p.transmitter_address) == 0) {
                host.pending.push_back(p.transmitter_address);
            }
        } else if (magic == kEapolLogoffMagic) {
            Host& host = hosts_[p.transmitter_address];
            EapolLogoff logoff;
            if (ParseEapolLogoffFrame(p.data, &logoff)) {
                host.max_nodes = logoff.max_nodes;
                host.total_nodes = logoff.connected_nodes;
            }
            if (p.destination_address == kBroadcastMac) {
                if (host.pending.empty()) {
                    Add("logoff-broadcast", "host " + Name(p.transmitter_address) +
                                                " broadcasts an EAPoL-Logoff that answers no first EAPoL-Start");
                } else {
                    host.admitted.insert(host.pending.front());
                    host.pending.pop_front();
                }
            }
        }
    }

    void InspectSecure(const WifiPacket& p) {
        SecureDataHeader h;
        if (!ParseSecureDataHeader(p.data, &h)) {
            Add("secure-shape", "SecureData frame shorter than its header, from " + Name(p.transmitter_address));
            return;
        }
        if (h.is_management) {
            Add("secure-management", "SecureData with the management bit (channel " +
                                         std::to_string(h.data_channel) + ", node " + std::to_string(h.src_node_id) +
                                         " -> " + std::to_string(h.dest_node_id) + ")");
        }
        const u8* raw = p.data.data() + kLlcHeaderSize;
        if (LoadBE16(raw + 2) != 0) {
            Add("secure-shape", "SecureData with aggregate count " + std::to_string(LoadBE16(raw + 2)) + " (Azahar emits 0)");
        }
        if (h.protocol_size != h.securedata_size + 4u || p.data.size() != kLlcHeaderSize + h.protocol_size) {
            Add("secure-shape", "SecureData sizes disagree: protocol " + std::to_string(h.protocol_size) +
                                    ", securedata " + std::to_string(h.securedata_size) + ", frame " +
                                    std::to_string(p.data.size()));
        }
    }

    mutable std::mutex m_;
    std::vector<Violation> violations_;
    std::set<Mac> known_;
    std::set<Mac> beaconed_;           // transmitters of a beacon
    std::set<std::pair<Mac, Mac>> talked_; // (from, to) of every frame but a Deauthentication
    std::map<Mac, Mac> last_deauth_;   // transmitter -> destination of its previous frame, if a Deauthentication
    std::map<Mac, Host> hosts_;
    std::size_t inspected_ = 0;
};

inline AzaharRules g_rules;

/// Ends a scenario: no frame may have broken an Azahar rule. Prints what did.
#define CHECK_RULES()                                                                              \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (g_rules.count() != 0) {                                                                \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: Azahar-peer rules broken:\n%s", __FILE__, __LINE__, \
                         g_rules.Report().c_str());                                                \
        }                                                                                          \
    } while (0)
