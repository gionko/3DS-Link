// A model of an AZAHAR emulator's Download Play HOST (dlp:SRVR, a game that offers its "fake client" session: every player has the game,
// nothing is downloaded), as the room sees it: Azahar's src/core/hle/service/dlp/dlp_srvr.cpp at commit 0ab6399c, transcribed (the copy
// read for the design is in analysis/luigi-azahar-2026-11-09/azahar-src). The tests of the real Download Play's SEARCHER
// (Tests/dlp_real_client_test.cpp, phase F4 of PROGETTO-DLP-VERO-1.1.3.md) put it in front of our RealDlpNet. Its radio is one of our
// UdsEngine (the in-memory room of Tests/mock_room.hpp), driven the way Azahar drives its own nwm; the bytes it sends are the capture's
// (Tests/dlp_real_luigi_golden.inc: the announcement of Prova-B and its dialogue packets, with the sequence, the client's id and the
// passphrase put in and the checksum made again with the capture's seed: nobody checks it).
//
// What it does, in Azahar's terms (line numbers of dlp_srvr.cpp):
//   StartHosting (271-310)        BeginHostingNetwork of the Download Play's wlancommid with NO application data; the id8, the network id
//                                 and the attribute word are whatever was on Azahar's stack (here: random, from the model's seed); the
//                                 announcement starts 1 ms later, then every 500 ms (546-587), to everybody, ALWAYS (not only to
//                                 spectators), until StartDistribution, EndHosting or Finalize; the connection manager's thread starts.
//   ServerConnectionManager (589-883)  every poll (100 ms; 0 ms while distributing: here 1 ms) for each client node: a new node needs the
//                                 Auth (with manual acceptance: only after AcceptClient), sent again at every poll until the AuthAck; after
//                                 the ack of StartDistribution without content the Finish every 100 ms; in DistributeDone and SentPassphrase
//                                 the notice every 100 ms. Then every packet in its bind (channel 2, any node): a size field that is not
//                                 the length -> skipped ("continue"); the AuthAck takes the client's id; each accepted answer and each
//                                 notice's answer with its 9 raises the client's sequence. When nobody needs the distribution any more:
//                                 NeedToSendPassphrase (8); once every client was sent the passphrase: Complete (9), the loop ends,
//                                 Unbind and DestroyNetwork WITHOUT a Deauthentication (nwm_uds.cpp:1111-1138).
//   AcceptClient (480-500)        the Auth to that node.
//   StartDistribution (414-440)   the announcement stops; StartDistribution (03) to every joined client, ONCE.
//   BeginGame (442-478)           the final with the passphrase (06, index 1) to every client, ONCE; -1 ("Already sent passphrase") when a
//                                 client was sent it already, -1 when one is not DistributeDone.
// Its checks, where Azahar would crash or misread (recorded as violations, never asserted here): an answer shorter than its structure with
// a coherent size field (GetPacketBody returns null and nobody checks: 767-769, 777-779, 817-818, 844-845).
// Options: `corrupt_frame` (a frame of the announcement with a wrong size field), `destroy_after_final_ms` (the network ends that long after BeginGame's final, from that thread: the "1 ms" of R2c, instead of
// the next poll), `game_beacon_after_ms` (that long after the end, the same MAC hosts the game's own network: another wlancommid, with
// application data), `child_tid` / `max_clients` (another game, another size).
#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "azl/dlp_real_wire.hpp"
#include "dlp_real_luigi_golden.inc"
#include "mock_room.hpp"

class AzaharDlpHost {
public:
    // DLP_Srvr_State (dlp_srvr.h:17-25)
    static constexpr u32 kInitialized = 1, kAccepting = 2, kWaitingForDistribution = 6, kNeedToSendPassphrase = 8, kComplete = 9;
    // ClientState (dlp_srvr.h:139-149)
    enum Cl : u8 { NotJoined, NeedsAuth, NeedsAuthAck, Accepted, NeedsDistributeAck, NeedsContent, DoesNotNeedContent, DistributeDone, SentPassphrase };
    static constexpr u32 kGameWlanCommId = 0x001D1A10;  // the game's own network after the Download Play (capture 1c, X:774.169)

    AzaharDlpHost(MockRoom& room, u8 last, const char* name, u64 child_tid = 0x00040001001D1A00ULL, u8 max_clients = 1, u32 seed = 1)
        : con(room, last), child_tid_(child_tid), max_clients_(max_clients), rng_(seed) {
        for (u32 i = 0; i < kNameUnits; ++i) {
            name_[i] = 0;
        }
        for (u32 i = 0; i < kNameUnits && name[i]; ++i) {
            name_[i] = static_cast<u8>(name[i]);
        }
        CHECK_EQ(e().InitializeForLobby(0xB0B0u + last, name_), 0u);  // (Azahar's nwm is up while the DLP service is)
        for (u32 i = 0; i < 5; ++i) {
            const DlpRealGolden& g = kDlpRealLuigiAnnounce[i];
            frames_[i].assign(g.bytes, g.bytes + g.size);
        }
        StoreBE64(frames_[0].data() + 0x10, child_tid_);
        frames_[0][0x20] = static_cast<u8>(max_clients_ + 1);
        dlp_real::Seal(kDlpRealLuigiSeed, frames_[0].data(), frames_[0].size());
        for (u32 i = 0; i < kMaxClients; ++i) {
            cl_[i].rate_ms = platform::NowMs();
        }
    }
    ~AzaharDlpHost() {
        Finalize();
        if (game_.joinable()) {
            game_.join();
        }
        e().Shutdown();
    }
    UdsEngine& e() {
        return *con.engine;
    }
    const Mac& mac() const {
        return con.mac;
    }

    /// StartHosting(manual, channel).
    void StartHosting(bool manual, u8 channel) {
        NetworkInfo ni;
        std::memset(&ni, 0, sizeof(ni));
        ni.max_nodes = static_cast<u8>(max_clients_ + 1);
        ni.channel = channel;
        ni.initialized = 1;
        StoreBE32(ni.wlan_comm_id_be, 0x2810);
        // (whatever was on Azahar's stack: the design's 1 - a client must read none of these)
        ni.id = static_cast<u8>(rng_());
        const u32 netid = static_cast<u32>(rng_());
        StoreBE32(ni.network_id_be, netid);
        const u16 attrs = static_cast<u16>(rng_());
        ni.attributes_be[0] = static_cast<u8>(attrs >> 8);
        ni.attributes_be[1] = static_cast<u8>(attrs);
        id8 = ni.id;
        network_id = netid;
        attributes = attrs;
        CHECK_EQ(e().BeginHostingNetwork(reinterpret_cast<const u8*>(&ni), sizeof(ni)), 0u);
        u32 ev = 0;
        CHECK_EQ(e().Bind(1, 0x3c00, 2, 0xFFFF, &ev), 0u);
        manual_ = manual;
        {
            std::lock_guard<std::mutex> l(m_);
            state_ = kAccepting;
            poll_ms_ = 100;
        }
        hosting_ = true;
        broadcasting_ = true;
        bcast_ = std::thread([this] { Broadcaster(); });
        manager_ = std::thread([this] { Manager(); });
    }
    /// AcceptClient(node): -1 for a node that is not there.
    int AcceptClient(u16 node) {
        std::lock_guard<std::mutex> l(m_);
        if (node < 2 || node > max_clients_ + 1 || cl_[node - 2].state == NotJoined) {
            return -1;
        }
        SendAuthLocked(node);
        return 0;
    }
    void StartDistribution() {
        std::lock_guard<std::mutex> l(m_);
        broadcasting_ = false;
        distributing_ = true;
        waiting_for_passphrase_ = false;
        poll_ms_ = 1;  // (dlp_poll_rate_distribute = 0: a busy loop)
        state_ = kWaitingForDistribution;
        for (u32 s = 0; s < max_clients_; ++s) {
            Client& cl = cl_[s];
            if (cl.state == NotJoined) {
                continue;
            }
            if (cl.state != Accepted) {
                ++errors_not_ready;  // "Client was not ready start distribution" (it is sent anyway)
            }
            cl.state = NeedsDistributeAck;
            SendLocked(static_cast<u16>(s + 2), Packet(kDlpRealLuigiDialog[4], cl), "A3");
        }
    }
    /// EndHosting (311-321): the announcement stops, the state is Initialized; the network and the connection manager go on.
    void EndHosting() {
        std::lock_guard<std::mutex> l(m_);
        broadcasting_ = false;
        state_ = kInitialized;
    }
    /// BeginGame(passphrase): 0, or -1.
    int BeginGame(const u8 pass[9]) {
        std::unique_lock<std::mutex> l(m_);
        for (u32 s = 0; s < max_clients_; ++s) {
            Client& cl = cl_[s];
            if (cl.state == NotJoined) {
                continue;
            }
            if (cl.state == SentPassphrase) {
                ++already_sent;  // "Already sent passphrase"
                return -1;
            }
            if (cl.state != DistributeDone) {
                return -1;
            }
            std::vector<u8> p = Packet(kDlpRealLuigiDialog[10], cl);
            std::memcpy(p.data() + 0x14, pass, 9);
            Reseal(&p);
            SendLocked(static_cast<u16>(s + 2), p, "A7");
            cl.state = SentPassphrase;
        }
        waiting_for_passphrase_ = true;
        ++begin_games;
        if (destroy_after_final_ms != 0) {
            l.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(destroy_after_final_ms));
            End();  // (silent from here: no answer, no notice, no Deauthentication)
        }
        return 0;
    }
    /// Finalize: the threads end, the network with them (no Deauthentication either).
    void Finalize() {
        broadcasting_ = false;
        hosting_ = false;
        if (bcast_.joinable()) {
            bcast_.join();
        }
        if (manager_.joinable()) {
            manager_.join();
        }
        End();
    }

    u32 state() {
        std::lock_guard<std::mutex> l(m_);
        return state_;
    }
    u8 client_state(u16 node) {
        std::lock_guard<std::mutex> l(m_);
        return node >= 2 && node <= max_clients_ + 1 ? cl_[node - 2].state : 0xFF;
    }
    u32 sent(const char* what) {
        std::lock_guard<std::mutex> l(m_);
        u32 n = 0;
        for (const std::string& s : log_) {
            n += s == what;
        }
        return n;
    }
    u32 received(u8 type) {
        std::lock_guard<std::mutex> l(m_);
        return type < 8 ? recv_[type] : 0;
    }
    std::vector<std::string> violations() {
        std::lock_guard<std::mutex> l(m_);
        return violations_;
    }
    bool ended() const {
        return ended_;
    }

    Console con;
    std::atomic<u32> destroy_after_final_ms{0};
    std::atomic<u32> game_beacon_after_ms{0};
    std::atomic<u32> groups{0};  // groups of the announcement sent
    std::atomic<int> corrupt_frame{-1};  // this frame of every group goes out with a wrong size field (set before StartHosting)
    u8 id8 = 0;
    u32 network_id = 0;
    u16 attributes = 0;
    std::atomic<u32> errors_not_ready{0}, already_sent{0}, begin_games{0}, size_skipped{0};

private:
    static constexpr u32 kMaxClients = 3;
    struct Client {
        u8 state = NotJoined;
        u8 seq = 0;
        u8 rid[2] = {};
        u64 rate_ms = 0;
    };
    static void StoreBE64(u8* p, u64 v) {
        StoreBE32(p, static_cast<u32>(v >> 32));
        StoreBE32(p + 4, static_cast<u32>(v));
    }
    static void Reseal(std::vector<u8>* p) {
        dlp_real::Seal(kDlpRealLuigiSeed, p->data(), p->size());
    }
    /// A packet of the capture with this client's {seq, rid} (GetPkRespId), the Auth keeping its default_resp_id.
    static std::vector<u8> Packet(const DlpRealGolden& g, const Client& cl) {
        std::vector<u8> p(g.bytes, g.bytes + g.size);
        if (p[0] != 2) {
            p[0x0D] = cl.seq;
            p[0x0E] = cl.rid[0];
            p[0x0F] = cl.rid[1];
        }
        Reseal(&p);
        return p;
    }
    bool ShouldRateLimit(Client& cl, u32 ms) {
        const u64 now = platform::NowMs();
        if (now - cl.rate_ms < ms) {
            return true;
        }
        cl.rate_ms = now;
        return false;
    }
    void SendLocked(u16 node, const std::vector<u8>& p, const char* what) {
        if (ended_) {
            return;
        }
        e().SendTo(node, 2, static_cast<u32>(p.size()), 0, p.data(), p.size());
        log_.push_back(what);
    }
    void SendAuthLocked(u16 node) {
        Client& cl = cl_[node - 2];
        SendLocked(node, Packet(kDlpRealLuigiDialog[0], cl), "A1");
        if (cl.state == NeedsAuth) {
            cl.state = NeedsAuthAck;
        }
    }
    void Violation(const std::string& s) {
        violations_.push_back(s);
    }
    /// The end of the hosting: Unbind, DestroyNetwork without a Deauthentication; then, if asked, the game's own network. Once.
    void End() {
        if (ended_.exchange(true)) {
            return;
        }
        broadcasting_ = false;
        hosting_ = false;
        e().Unbind(1);
        e().SetDeauthOnDestroy(false);
        e().DestroyNetwork();
        const u32 after = game_beacon_after_ms;
        if (after != 0) {
            game_ = std::thread([this, after] {
                std::this_thread::sleep_for(std::chrono::milliseconds(after));
                NetworkInfo ni;
                std::memset(&ni, 0, sizeof(ni));
                ni.max_nodes = static_cast<u8>(max_clients_ + 1);
                ni.channel = 6;
                ni.initialized = 1;
                StoreBE32(ni.wlan_comm_id_be, kGameWlanCommId);
                ni.id = 0x55;
                StoreBE32(ni.network_id_be, static_cast<u32>(rng_()));
                ni.application_data_size = 8;
                for (u32 i = 0; i < 8; ++i) {
                    ni.application_data[i] = static_cast<u8>(0xA0 + i);
                }
                CHECK_EQ(e().BeginHostingNetwork(reinterpret_cast<const u8*>(&ni), sizeof(ni)), 0u);
            });
        }
    }
    void Broadcaster() {
        u64 next = platform::NowMs() + 1;  // (title_broadcast_delay_ms)
        while (broadcasting_) {
            if (platform::NowMs() < next) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            next += 500;
            for (u32 i = 0; i < 5 && broadcasting_; ++i) {
                std::vector<u8> f = frames_[i];
                if (static_cast<int>(i) == corrupt_frame) {
                    f[5] = static_cast<u8>(f[5] + 1);  // (its size field one more than its length: the test of a malformed announcement)
                }
                e().SendTo(0xFFFF, 1, static_cast<u32>(f.size()), 0, f.data(), f.size());
            }
            ++groups;
        }
    }
    void Manager() {
        std::vector<u8> buf(0x3c00);
        for (;;) {
            u32 poll;
            {
                std::lock_guard<std::mutex> l(m_);
                poll = poll_ms_;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(poll));
            if (!hosting_) {
                break;
            }
            std::lock_guard<std::mutex> l(m_);
            u16 me = 0, mask = 0;
            e().NodeSnapshot(&me, &mask);
            for (u32 s = 0; s < max_clients_; ++s) {
                Client& cl = cl_[s];
                const u16 node = static_cast<u16>(s + 2);
                const bool present = (mask & (1u << (node - 1))) != 0;
                if (cl.state != NotJoined && !present) {
                    cl.state = NotJoined;  // "Client disconnected"
                }
                if (cl.state == NotJoined) {
                    if (!present) {
                        continue;
                    }
                    cl.state = NeedsAuth;
                    cl.seq = 0;
                }
                if (cl.state == NeedsAuth && manual_) {
                    continue;
                }
                if (cl.state == NeedsAuth || cl.state == NeedsAuthAck) {
                    SendAuthLocked(node);
                } else if (cl.state == DoesNotNeedContent) {
                    if (!ShouldRateLimit(cl, 100)) {
                        std::vector<u8> p = Packet(kDlpRealLuigiDialog[8], cl);  // (seq_num 0: the no-content Finish)
                        SendLocked(node, p, "A5");
                    }
                } else if (cl.state == DistributeDone || cl.state == SentPassphrase) {
                    if (!ShouldRateLimit(cl, 100)) {
                        SendLocked(node, Packet(kDlpRealLuigiDialog[11], cl), "A8");
                    }
                }
            }
            for (;;) {
                u32 n = 0;
                u16 src = 0;
                if (e().PullPacket(1, 0x3c00, buf.data(), static_cast<u32>(buf.size()), &n, &src) != 0 || n == 0) {
                    break;
                }
                if (src < 2 || src > max_clients_ + 1u) {
                    continue;  // "Could not get client state from received packet's node id"
                }
                Client& cl = cl_[src - 2];
                if (n < 16 || static_cast<u32>((buf[4] << 8) | buf[5]) != n) {
                    ++size_skipped;  // "Could not validate DLP packet header": continue
                    continue;
                }
                const u8 type = buf[0];
                if (type < 8) {
                    ++recv_[type];
                }
                if (type == 2) {
                    if (cl.state != NeedsAuthAck) {
                        continue;
                    }
                    if (n < 0x18) {
                        Violation("an AuthAck shorter than 0x18 (Azahar reads a null body)");
                        continue;
                    }
                    cl.state = Accepted;
                    cl.rid[0] = buf[0x16];
                    cl.rid[1] = buf[0x17];
                    ++cl.seq;
                } else if (type == 3) {
                    if (cl.state != NeedsDistributeAck) {
                        continue;
                    }
                    if (n < 0x18) {
                        Violation("an ack of StartDistribution shorter than 0x18 (Azahar reads a null body)");
                        continue;
                    }
                    if (buf[0x15] == 0x20) {
                        Violation("a client asked for the content (a download: Azahar would send the child)");
                        cl.state = NeedsContent;
                    } else {
                        cl.state = DoesNotNeedContent;
                    }
                    ++cl.seq;
                } else if (type == 5) {
                    if (cl.state != DoesNotNeedContent) {
                        continue;
                    }
                    if (n < 0x1C) {
                        Violation("a Finish ack shorter than 0x1C (Azahar reads a null body)");
                        continue;
                    }
                    if (buf[0x15] == 0) {
                        cl.state = DistributeDone;  // "Client has finished downloading content"
                    }
                    ++cl.seq;
                } else if (type == 6) {
                    if (cl.state != DistributeDone && cl.state != SentPassphrase) {
                        continue;
                    }
                    if (n < 0x18) {
                        Violation("a notice ack shorter than 0x18 (Azahar reads a null body)");
                        continue;
                    }
                    if (buf[0x14] != 9) {
                        continue;  // "Client BeginGameAck unk2 is not 0x9 yet"
                    }
                    ++cl.seq;
                }
            }
            bool any_needs = false, all_got = true;
            for (u32 s = 0; s < max_clients_; ++s) {
                const u8 st = cl_[s].state;
                any_needs |= st == NeedsContent || st == DoesNotNeedContent || st == NeedsDistributeAck;
                all_got &= st == NotJoined || st == SentPassphrase;
            }
            if (distributing_ && !any_needs) {
                distributing_ = false;
                state_ = kNeedToSendPassphrase;
                waiting_for_passphrase_ = true;
                poll_ms_ = 100;
            } else if (waiting_for_passphrase_ && all_got) {
                waiting_for_passphrase_ = false;
                state_ = kComplete;
                hosting_ = false;
                for (u32 s = 0; s < max_clients_; ++s) {
                    cl_[s].state = NotJoined;
                }
            }
        }
        End();
    }

    u16 name_[kNameUnits];
    u64 child_tid_;
    u32 max_clients_;
    std::mt19937 rng_;
    std::vector<u8> frames_[5];
    std::mutex m_;
    u32 state_ = kInitialized;
    u32 poll_ms_ = 100;
    bool manual_ = false;
    bool distributing_ = false;
    bool waiting_for_passphrase_ = false;
    Client cl_[kMaxClients];
    std::vector<std::string> log_;
    std::vector<std::string> violations_;
    u32 recv_[8] = {};
    std::atomic<bool> hosting_{false}, broadcasting_{false}, ended_{false};
    std::thread bcast_, manager_, game_;
};
