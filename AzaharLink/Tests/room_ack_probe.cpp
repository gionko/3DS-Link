// room_ack_probe: two members of a REAL azahar-room with the plugin's ENet host, the way a game uses the room - one sends a frame every
// 1000/fps ms (broadcast, ENet UNSEQUENCED like radio_like=1), the other takes each frame, works on it for `handler_ms` (the engine, the
// capture of debug=1) and answers with a frame of its own. What it measures is what the change of the order of the acknowledgement and
// the packet (and of the other changes of the data path) does against the REAL room, with the room's own ENet 1.3.18 timers:
//   * the delay of a frame from the sender's send to the start of the receiver's work on it (both in this process: one clock),
//   * the datagrams the receiver got beyond the frames (the room's retransmissions - an acknowledgement that came too late - and its pings),
//   * the datagrams the receiver sent: acknowledgement-only, acknowledgement with data (the acknowledgement riding on the answer), data-only.
// It uses nothing but ENet and the room's message encoders, so the same program is built against the original ENet and the patched
// one (Tests/room_ack.sh), and a delay in a user-space proxy between the members and the room (Tests/udp_gate.cpp: the Docker Desktop kernel
// has no netem) makes the wire a LAN's (8 ms round trip, a few ms of jitter). The proxy tells the members apart by the port they talk to, so the
// receiver may have a port of its own.
//   room_ack_probe <host> <port> [seconds=20] [fps=60] [handler_ms=0] [answer=1] [label] [receiver_port=port]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <enet/enet.h>

#include "azl/room_proto.hpp"
#include "azl/uds_frames.hpp"
#include "support.hpp"

using namespace azl;
using Clock = std::chrono::steady_clock;

// the datagrams the receiver sends, classified by the link (-Wl,--wrap=enet_socket_send): the wrap sees every host of the process, the
// tag of the calling thread says whose it is
static thread_local int t_who = 0; // 1 = the receiver
static std::atomic<unsigned long> g_ack_only{0}, g_ack_data{0}, g_data_only{0};
static std::atomic<unsigned long> g_recv_data{0}, g_recv_empty{0}; // the receive calls of the receiver: with a datagram, and the ones that only say "nothing"
extern "C" int __real_enet_socket_receive(ENetSocket, ENetAddress*, ENetBuffer*, size_t);
extern "C" int __wrap_enet_socket_receive(ENetSocket s, ENetAddress* a, ENetBuffer* b, size_t n) {
    const int r = __real_enet_socket_receive(s, a, b, n);
    if (t_who == 1) {
        if (r > 0) {
            ++g_recv_data;
        } else if (r == 0) {
            ++g_recv_empty;
        }
    }
    return r;
}
extern "C" size_t enet_protocol_command_size(enet_uint8);
extern "C" int __real_enet_socket_send(ENetSocket, const ENetAddress*, const ENetBuffer*, size_t);
extern "C" int __wrap_enet_socket_send(ENetSocket s, const ENetAddress* a, const ENetBuffer* b, size_t n) {
    if (t_who == 1) {
        unsigned char g[1600];
        size_t total = 0;
        for (size_t i = 0; i < n && total + b[i].dataLength <= sizeof g; ++i) {
            std::memcpy(g + total, b[i].data, b[i].dataLength);
            total += b[i].dataLength;
        }
        if (total >= 4) {
            const unsigned peer_id = static_cast<unsigned>((g[0] << 8) | g[1]);
            size_t i = (peer_id & 0x8000) ? 4 : 2;
            int acks = 0, other = 0;
            while (i + 4 <= total) {
                const unsigned cmd = g[i] & 0x0F;
                if (cmd == 0 || cmd > 12) {
                    break;
                }
                const size_t sz = enet_protocol_command_size(static_cast<enet_uint8>(cmd));
                if (!sz || i + sz > total) {
                    break;
                }
                size_t dl = 0;
                if (cmd == 6) {
                    dl = static_cast<size_t>((g[i + 4] << 8) | g[i + 5]);
                } else if (cmd == 7 || cmd == 8 || cmd == 9 || cmd == 12) {
                    dl = static_cast<size_t>((g[i + 6] << 8) | g[i + 7]);
                }
                (cmd == 1 ? acks : other)++;
                i += sz + dl;
            }
            if (acks && !other) {
                ++g_ack_only;
            } else if (acks) {
                ++g_ack_data;
            } else {
                ++g_data_only;
            }
        }
    }
    return __real_enet_socket_send(s, a, b, n);
}

namespace {

double NowMs() {
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

struct Member {
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    Mac mac{};
    bool Join(const std::string& address, u16 port, const std::string& nick) {
        host = enet_host_create(nullptr, 1, room::kNumChannels, 0, 0);
        if (!host) {
            return false;
        }
        ENetAddress a{};
        enet_address_set_host(&a, address.c_str());
        a.port = port;
        peer = enet_host_connect(host, &a, room::kNumChannels, 0);
        ENetEvent ev;
        bool up = false;
        for (int i = 0; i < 100 && !up; ++i) {
            up = enet_host_service(host, &ev, 100) > 0 && ev.type == ENET_EVENT_TYPE_CONNECT;
        }
        if (!up) {
            return false;
        }
        enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL, ENET_PEER_PACKET_THROTTLE_SCALE, 0);
        const std::vector<u8> info = room::EncodeSetGameInfo("Test Game", 0x0004000000164800ULL);
        const std::vector<u8> join = room::EncodeJoinRequest(nick, "probe-" + nick, kNoPreferredMac, "", "");
        enet_peer_send(peer, 0, enet_packet_create(join.data(), join.size(), ENET_PACKET_FLAG_RELIABLE));
        enet_peer_send(peer, 0, enet_packet_create(info.data(), info.size(), ENET_PACKET_FLAG_RELIABLE));
        enet_host_flush(host);
        for (int i = 0; i < 100; ++i) {
            if (enet_host_service(host, &ev, 100) > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) {
                const bool ok = ev.packet->dataLength > 0 && ev.packet->data[0] == room::IdJoinSuccess &&
                                room::DecodeJoinSuccess(ev.packet->data, ev.packet->dataLength, &mac);
                enet_packet_destroy(ev.packet);
                if (ok) {
                    return true;
                }
            }
        }
        return false;
    }
};

std::vector<u8> Frame(const Mac& from, u32 seq, double sent_ms) {
    WifiPacket p;
    p.type = WifiPacket::Type::Data;
    p.channel = 11;
    p.transmitter_address = from;
    p.destination_address = kBroadcastMac;
    std::vector<u8> body(140, 0x5A);
    std::memcpy(body.data(), &seq, 4);
    std::memcpy(body.data() + 4, &sent_ms, 8);
    p.data = GenerateDataPayload(body.data(), body.size(), 1, kBroadcastNetworkNodeId, 2, static_cast<u16>(seq));
    return room::EncodeWifiPacket(p);
}

double Pct(std::vector<double>& v, double p) {
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))];
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <host> <port> [seconds] [fps] [handler_ms] [answer] [label] [receiver_port]\n", argv[0]);
        return 2;
    }
    const std::string host = argv[1];
    const u16 port = static_cast<u16>(std::atoi(argv[2]));
    const double seconds = argc > 3 ? std::atof(argv[3]) : 20;
    const double fps = argc > 4 ? std::atof(argv[4]) : 60;
    const double handler_ms = argc > 5 ? std::atof(argv[5]) : 0;
    const bool answer = argc > 6 ? std::atoi(argv[6]) != 0 : true;
    const std::string label = argc > 7 ? argv[7] : "run";
    const u16 receiver_port = argc > 8 ? static_cast<u16>(std::atoi(argv[8])) : port;
    enet_initialize();
    Member a, b;
    if (!a.Join(host, port, "ProbeSender") || !b.Join(host, receiver_port, "ProbeRecv")) {
        std::printf("NOT JOINED\n");
        return 1;
    }
    std::atomic<bool> stop{false};
    std::mutex m;
    std::vector<double> latency;
    std::atomic<unsigned long> sent{0}, got{0};
    // the receiver: its own thread, its own host
    unsigned long dgrams_in_before = 0, dgrams_out_before = 0;
#ifdef P5A_COUNTERS
    ENetAzlCounters counters{};
#endif
    std::thread rx([&] {
        t_who = 1;
        dgrams_in_before = b.host->totalReceivedPackets;
        dgrams_out_before = b.host->totalSentPackets;
        u32 reply_seq = 0;
        while (!stop.load()) {
            ENetEvent ev;
            const int r = enet_host_service(b.host, &ev, 3);
            if (r > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) {
                WifiPacket p;
                if (ev.packet->dataLength > 0 && ev.packet->data[0] == room::IdWifiPacket && room::DecodeWifiPacket(ev.packet->data, ev.packet->dataLength, &p) &&
                    p.data.size() >= 8 + kLlcHeaderSize + kSecureDataHeaderSize + 12 && p.transmitter_address == a.mac) {
                    double sent_ms = 0;
                    std::memcpy(&sent_ms, p.data.data() + kLlcHeaderSize + kSecureDataHeaderSize + 4, 8);
                    const double t = NowMs();
                    {
                        std::lock_guard<std::mutex> l(m);
                        latency.push_back(t - sent_ms);
                    }
                    ++got;
                    if (handler_ms > 0) { // the work on the frame: the engine, the capture
                        const auto until = Clock::now() + std::chrono::microseconds(static_cast<long>(handler_ms * 1000));
                        while (Clock::now() < until) {
                        }
                    }
                    if (answer) {
                        const std::vector<u8> reply = Frame(b.mac, ++reply_seq, NowMs());
                        enet_peer_send(b.peer, 0, enet_packet_create(reply.data(), reply.size(), ENET_PACKET_FLAG_UNSEQUENCED | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT));
                        enet_host_flush(b.host);
                    }
                }
                enet_packet_destroy(ev.packet);
            } else if (r > 0 && ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                std::printf("RECEIVER DISCONNECTED\n");
                break;
            }
        }
    });
    // the sender: a frame every 1000/fps ms
    std::thread tx([&] {
        t_who = 0;
        u32 seq = 0;
        const auto t0 = Clock::now();
        while (!stop.load()) {
            const double due_ms = static_cast<double>(seq) * 1000.0 / fps;
            const double now_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            if (now_ms >= due_ms) {
                const std::vector<u8> f = Frame(a.mac, ++seq, NowMs());
                enet_peer_send(a.peer, 0, enet_packet_create(f.data(), f.size(), ENET_PACKET_FLAG_UNSEQUENCED | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT));
                enet_host_flush(a.host);
                ++sent;
            }
            ENetEvent ev;
            if (enet_host_service(a.host, &ev, 1) > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(ev.packet);
            }
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(seconds * 1000)));
    stop = true;
    rx.join();
    tx.join();
#ifdef P5A_COUNTERS
    enet_azl_counters_take(b.host, &counters);
#endif
    const unsigned long dgrams_in = b.host->totalReceivedPackets - dgrams_in_before, dgrams_out = b.host->totalSentPackets - dgrams_out_before;
    std::vector<double> lat;
    {
        std::lock_guard<std::mutex> l(m);
        lat = latency;
    }
    const double mean = lat.empty() ? 0 : [&] { double s = 0; for (double x : lat) s += x; return s / static_cast<double>(lat.size()); }();
    std::printf("%s: fps %.0f handler %.1f ms answer %d, %.0f s | frames sent %lu, got %lu | delay ms mean %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f |"
                " receiver datagrams in %lu (frames %lu, beyond %lu = %.1f/s: the room's retransmissions and pings), out %lu (ack-only %lu, ack+data %lu, data-only %lu)\n",
                label.c_str(), fps, handler_ms, answer ? 1 : 0, seconds, sent.load(), got.load(), mean, Pct(lat, 0.5), Pct(lat, 0.95), Pct(lat, 0.99), Pct(lat, 1.0),
                dgrams_in, got.load(), dgrams_in > got.load() ? dgrams_in - got.load() : 0ul, dgrams_in > got.load() ? static_cast<double>(dgrams_in - got.load()) / seconds : 0.0,
                dgrams_out, g_ack_only.load(), g_ack_data.load(), g_data_only.load());
    std::printf("   receiver's receive calls: %lu with a datagram, %lu that only said nothing (%.2f per datagram)\n", g_recv_data.load(), g_recv_empty.load(),
                g_recv_data.load() ? static_cast<double>(g_recv_empty.load()) / static_cast<double>(g_recv_data.load()) : 0.0);
#ifdef P5A_COUNTERS
    std::printf("   ENet counters of the receiver: out of order %u (waited up to %u ms), resent copies received %u, ack-only %u, ack+data %u, passes %u (longest %u datagrams, %u ms, %u cut short),"
                " receives skipped %u, handed over before the ack %u (limit hit %u, longest hold %u ms)\n",
                counters.oooCommands, counters.holWaitMaxMs, counters.dupCommands, counters.ackOnlyDatagrams, counters.ackDataDatagrams, counters.passes,
                counters.passDatagramsMax, counters.passMsMax, counters.passesCapped, counters.recvSkipped, counters.eventsBeforeAck, counters.ackHoldLimitHits,
                counters.ackHoldMaxMs);
#endif
    enet_peer_disconnect_now(a.peer, 0);
    enet_peer_disconnect_now(b.peer, 0);
    enet_host_flush(a.host);
    enet_host_flush(b.host);
    return 0;
}
