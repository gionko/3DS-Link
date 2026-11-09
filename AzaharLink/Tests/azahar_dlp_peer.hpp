// A model of an AZAHAR emulator's Download Play CLIENT (dlp:FKCL, the "fake client": it has the game, it downloads nothing), as the
// room sees it: Azahar's src/core/hle/service/dlp/dlp_clt_base.cpp at commit 0ab6399c, transcribed (the copy read for the design is in
// analysis/luigi-azahar-2026-11-09/azahar-src). The tests of the real Download Play's host (Tests/dlp_real_net_test.cpp, phase F3 of
// PROGETTO-DLP-VERO-1.1.3.md) put it in front of our RealDlpNet. Its radio is one of our UdsEngine (the in-memory room of
// Tests/mock_room.hpp), driven the way Azahar drives its own nwm: the model's logic is Azahar's, the frames are our engine's.
//
// What it does, in Azahar's terms (line numbers of dlp_clt_base.cpp / dlp_base.cpp):
//   Probe (CacheBeaconTitleInfo, 492-570)  connect to the host as a SPECTATOR (channel of its own, 11 as in the capture), a busy wait of
//                                          at most 3000 ms (dlp_base.cpp:110-121), then a bind of channel 1 from node 1 and 1000 ms of real
//                                          time FROM THE BIND to collect the 5 frames: a frame whose size field is not its length or
//                                          whose index is >= 5 puts the host on the ignored list and ends it (534-537); with the 5, the
//                                          host's name is node 1's (543-544: Azahar dereferences it, a host whose Logoff did not list
//                                          node 1 crashes it); then Unbind and Disconnect (one Deauthentication); no frame at all = ignored.
//   Session (StartSession 308-366, ClientConnectionManager 655-813)  connect as a CLIENT (3 s), state 5; a bind of channel 2 from
//                                          node 1; then ONE packet per poll: 100 ms until the StartDistribution, 0 ms after it, 100 ms
//                                          again at the first notice; a packet whose size field is not its length ends the session
//                                          (682-686); the answers copy +0x0D..0F of the packet and put index 0: Auth -> AuthAck (rid
//                                          01 02, in any state), StartDistribution -> the ack without content (state 7), Finish (index 1
//                                          only) -> its ack, Notice (6, index 0) -> 9, the final (6, index 1) -> state 9 and the passphrase.
//   StopSession / Finalize                  the session's thread ends: Unbind, Disconnect.
// Its checks, where Azahar would crash or misread (recorded as violations, never asserted here): a frame of the announcement shorter
// than its structure (768 / 1448), a Finish shorter than 0x18 or a final shorter than 0x20 (GetPacketBody returns null, nobody
// checks), node 1 without information while the probe reads its name.
// Options for the scenarios: `rtt_ms` (an answer leaves that long after its packet was taken: the round trip of a far room),
// `answer_*` false (that answer is never sent), `want_content` (the ack of StartDistribution asks for the content: a download).
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mock_room.hpp"

class AzaharDlpClient {
public:
    // DLP_Clt_State (dlp_base.h:62-72)
    static constexpr u32 kInitialized = 1, kScanning = 2, kJoined = 5, kWaitingForServerReady = 7, kComplete = 9;

    struct Probe {
        bool connected = false;  // the spectator's connection was answered within 3 s
        bool all = false;        // the 5 frames within 1000 ms of the bind
        bool ignored = false;    // the host went on the ignored list (a bad frame, or nothing at all)
        u32 mask = 0;            // the indexes received
        u32 ms_to_all = 0;       // from the bind to the fifth frame
        u64 child_tid = 0;       // frame 0, +0x10 (big-endian)
        u8 max_nodes = 0;        // frame 0, +0x20
        u16 name[kNameUnits] = {};  // node 1's
    };

    /// `mac`: the address the model speaks from (Tests/dlp_real_room_probe.cpp, I2: the MAC the REAL room gave the ENet member that carries
    /// the model's frames; the in-memory room is then only the model's own console, and the probe forwards what it sends). Default: the
    /// in-memory room's address of `last`.
    AzaharDlpClient(MockRoom& room, u8 last, const char* name, u8 channel = 11, const Mac* mac = nullptr) : con(room, last), channel_(channel) {
        if (mac != nullptr) {
            con.mac = *mac;  // (the console's transport reads it at every send and get_mac: set before the engine is initialised)
        }
        for (u32 i = 0; i < kNameUnits; ++i) {
            name_[i] = 0;
        }
        for (u32 i = 0; i < kNameUnits && name[i]; ++i) {
            name_[i] = static_cast<u8>(name[i]);
        }
        // (Azahar's nwm is up while the DLP service is: InitializeCltBase)
        CHECK_EQ(e().InitializeForLobby(0xA0A0u + last, name_), 0u);
        CHECK(e().PrepareLobby());
        e().SetLobbyBeaconKind(UdsEngine::LobbyBeaconKind::RealDlp);
        state_ = kInitialized;
    }
    ~AzaharDlpClient() {
        StopSession();
        e().Shutdown();
    }
    UdsEngine& e() {
        return *con.engine;
    }
    const Mac& mac() const {
        return con.mac;
    }
    bool Heard(const Mac& host) {
        UdsEngine::HeardLobby h;
        for (u32 i = 0; e().HeardLobbyAt(i, 1000, &h); ++i) {
            if (h.host == host) {
                return true;
            }
        }
        return false;
    }

    /// CacheBeaconTitleInfo for `host` (synchronous: at most 3 s + 1 s; the caller pumps the host meanwhile from another thread).
    Probe ProbeHost(const Mac& host) {
        Probe out;
        state_ = kScanning;
        if (e().ConnectBeginDlp(host, channel_, ConnectionType::Spectator) != 0) {
            Violation("probe: the connect was refused at once");
            return out;
        }
        const u64 t_conn = platform::NowMs();
        while (e().ConnectPoll() != 1 || e().status() != NetworkStatus::ConnectedAsSpectator) {
            if (platform::NowMs() - t_conn > 3000) {
                e().DisconnectNetwork();  // "Timed out when trying to connect to beacon"; NOT ignored (504-508)
                ++timeouts;
                return out;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        out.connected = true;
        u32 ev = 0;
        CHECK_EQ(e().Bind(1, 0x3c00, 1, 1, &ev), 0u);
        const u64 t_bind = platform::NowMs();
        std::vector<u8> buf(0x3c00);
        while (platform::NowMs() - t_bind < 1000) {
            u32 n = 0;
            u16 src = 0;
            if (e().PullPacket(1, 0x3c00, buf.data(), static_cast<u32>(buf.size()), &n, &src) != 0 || n == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            ++probe_frames;
            const u32 idx = n >= 16 ? buf[12] : 0xFF;
            if (n < 16 || Be16(&buf[4]) != n || idx >= 5) {
                out.ignored = true;  // corrupted info
                break;
            }
            if ((idx == 0 && n < 768) || (idx >= 1 && idx <= 3 && n < 1448)) {
                Violation("probe: frame " + std::to_string(idx) + " shorter than its structure (" + std::to_string(n) + " bytes)");
            }
            if (idx == 0 && n >= 0x21) {
                out.child_tid = 0;
                for (u32 i = 0; i < 8; ++i) {
                    out.child_tid = (out.child_tid << 8) | buf[0x10 + i];
                }
                out.max_nodes = buf[0x20];
            }
            out.mask |= 1u << idx;
            if (out.mask == 0x1F) {
                out.all = true;
                out.ms_to_all = static_cast<u32>(platform::NowMs() - t_bind);
                NodeInfo info;
                if (e().GetNodeInformation(1, &info) != 0) {
                    Violation("probe: node 1 has no information (Azahar dereferences it)");
                } else {
                    std::memcpy(out.name, info.username, sizeof(out.name));
                }
                break;
            }
        }
        e().Unbind(1);
        e().DisconnectNetwork();
        if (!out.all && out.mask == 0) {
            out.ignored = true;
        }
        state_ = kInitialized;  // (StopScan)
        return out;
    }

    /// StartSession: the connect as a client (3 s), then the session's thread. False = the connect failed.
    bool StartSession(const Mac& host) {
        state_ = kJoined;
        if (e().ConnectBeginDlp(host, channel_, ConnectionType::Client) != 0) {
            return false;
        }
        const u64 t0 = platform::NowMs();
        while (e().ConnectPoll() != 1 || e().status() != NetworkStatus::ConnectedAsClient) {
            if (platform::NowMs() - t0 > 3000) {
                e().DisconnectNetwork();
                return false;  // "Could not connect to dlp server (timed out)"
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        connected_ = true;
        thread_ = std::thread([this] { Manager(); });
        return true;
    }
    /// StopSession / Finalize: the thread ends (Unbind, Disconnect: one Deauthentication).
    void StopSession() {
        connected_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    u32 state() const {
        return state_;
    }
    std::vector<u8> passphrase() {
        std::lock_guard<std::mutex> l(m_);
        return pass_;
    }
    std::vector<std::string> violations() {
        std::lock_guard<std::mutex> l(m_);
        return violations_;
    }
    /// When (platform::NowMs) the packets of each type were first taken: [type][0] for index 0, [type][1] for index 1.
    u64 first_ms(u8 type, u8 idx) {
        std::lock_guard<std::mutex> l(m_);
        return type < 8 && idx < 2 ? first_[type][idx] : 0;
    }
    u32 received(u8 type, u8 idx) {
        std::lock_guard<std::mutex> l(m_);
        return type < 8 && idx < 2 ? recv_[type][idx] : 0;
    }

    Console con;
    std::atomic<u32> rtt_ms{0};
    std::atomic<bool> answer_auth{true}, answer_dist{true}, answer_finish{true}, answer_notice{true}, want_content{false};
    std::atomic<u32> timeouts{0};
    std::atomic<u32> probe_frames{0};
    std::atomic<bool> session_broken{false};  // a packet whose size field was not its length ended the session

private:
    static u16 Be16(const u8* p) {
        return static_cast<u16>((p[0] << 8) | p[1]);
    }
    void Violation(const std::string& s) {
        std::lock_guard<std::mutex> l(m_);
        violations_.push_back(s);
    }
    /// An answer (PGen_SetPK: the header of the type, index 0, the resp_id of the packet answered) of `size` bytes.
    std::vector<u8> Answer(const std::vector<u8>& got, u8 type, u32 size) {
        std::vector<u8> a(size, 0);
        a[0] = type;
        a[1] = 0x02;
        a[4] = static_cast<u8>(size >> 8);
        a[5] = static_cast<u8>(size);
        a[6] = 0x02;
        a[12] = 0;
        a[13] = got[13];
        a[14] = got[14];
        a[15] = got[15];
        a[8] = 0x5A;  // (a checksum of Azahar's seed: nobody checks it)
        return a;
    }
    void Queue(std::vector<u8> a) {
        pending_.push_back({platform::NowMs() + rtt_ms.load(), std::move(a)});
    }
    void Flush() {
        const u64 now = platform::NowMs();
        for (std::size_t i = 0; i < pending_.size();) {
            if (pending_[i].first <= now) {
                e().SendTo(1, 2, static_cast<u32>(pending_[i].second.size()), 0, pending_[i].second.data(), pending_[i].second.size());
                pending_.erase(pending_.begin() + static_cast<long>(i));
            } else {
                ++i;
            }
        }
    }
    void Manager() {
        u32 ev = 0;
        CHECK_EQ(e().Bind(1, 0x3c00, 2, 1, &ev), 0u);
        u32 poll = 100;
        std::vector<u8> buf(0x3c00);
        const u64 tick_ms = 2;  // (the answers that wait for the round trip are flushed at this rate; Azahar's 0 ms is a busy loop)
        u64 next_pull = platform::NowMs() + poll;
        while (connected_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(tick_ms));
            Flush();
            if (platform::NowMs() < next_pull) {
                continue;
            }
            u32 n = 0;
            u16 src = 0;
            const bool got = e().PullPacket(1, 0x3c00, buf.data(), static_cast<u32>(buf.size()), &n, &src) == 0 && n != 0;
            next_pull = platform::NowMs() + poll;
            if (!got) {
                continue;
            }
            std::vector<u8> pk(buf.begin(), buf.begin() + n);
            if (n < 16 || Be16(&pk[4]) != n) {
                session_broken = true;  // "Could not validate DLP packet header": out of the loop
                break;
            }
            const u8 type = pk[0], idx = pk[12];
            {
                std::lock_guard<std::mutex> l(m_);
                if (type < 8 && idx < 2) {
                    if (recv_[type][idx]++ == 0) {
                        first_[type][idx] = platform::NowMs();
                    }
                }
            }
            if (type == 2) {
                if (answer_auth) {
                    std::vector<u8> a = Answer(pk, 2, 0x18);
                    a[0x10] = 1;
                    a[0x16] = 0x01;
                    a[0x17] = 0x02;
                    Queue(std::move(a));
                }
            } else if (type == 3) {
                poll = 0;
                if (!answer_dist) {
                    continue;
                }
                if (!want_content) {
                    std::vector<u8> a = Answer(pk, 3, 0x18);
                    a[0x10] = 1;
                    Queue(std::move(a));
                    state_ = kWaitingForServerReady;
                } else {
                    std::vector<u8> a = Answer(pk, 3, 0x38);
                    a[0x10] = 1;
                    a[0x15] = 0x20;  // (u16 BE 0x0020: the content is needed)
                    a[0x18] = 1;
                    Queue(std::move(a));
                    state_ = 6;  // Downloading
                }
            } else if (type == 5) {
                if (idx != 1) {
                    continue;  // "Received finish dist packet, but packet index was ..."
                }
                if (n < 0x18) {
                    Violation("session: a Finish shorter than 0x18 (Azahar reads a null body)");
                }
                if (answer_finish) {
                    std::vector<u8> a = Answer(pk, 5, 0x1C);
                    a[0x10] = 1;
                    a[0x14] = 1;  // finished_cur_block
                    a[0x15] = 0;  // needs_content
                    Queue(std::move(a));
                }
            } else if (type == 6) {
                if (idx == 0) {
                    poll = 100;
                    if (answer_notice) {
                        std::vector<u8> a = Answer(pk, 6, 0x18);
                        a[0x10] = 1;
                        a[0x14] = 9;
                        Queue(std::move(a));
                    }
                } else if (idx == 1) {
                    if (n < 0x20) {
                        Violation("session: a final shorter than 0x20 (Azahar reads a null body)");
                        continue;
                    }
                    std::lock_guard<std::mutex> l(m_);
                    state_ = kComplete;
                    pass_.assign(pk.begin() + 0x14, pk.begin() + 0x14 + 9);
                }
            }
        }
        Flush();
        e().Unbind(1);
        e().DisconnectNetwork();
    }

    u8 channel_;
    u16 name_[kNameUnits];
    std::atomic<u32> state_{0};
    std::atomic<bool> connected_{false};
    std::thread thread_;
    std::vector<std::pair<u64, std::vector<u8>>> pending_;  // (the session's thread only)
    std::mutex m_;
    std::vector<u8> pass_;
    std::vector<std::string> violations_;
    u32 recv_[8][2] = {};
    u64 first_[8][2] = {};
};
