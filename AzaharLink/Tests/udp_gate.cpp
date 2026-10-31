// udp_gate: a user-space UDP impairment proxy between the clients and the room (the Docker Desktop kernel has no sch_netem). From the analysis
// P3 of 2026-10-25 (fixes/P3/bench/gate.cpp), unchanged but for this line. Used by Tests/room_ack.sh.
//
//
//   udp_gate --room HOST:PORT --client LISTEN_PORT [--client ...] [options]
//
// For every --client a socket listens on LISTEN_PORT (the client connects to the gate as if it were the room) and a second socket,
// connected to the room, carries the client's datagrams upstream. Each direction has its own impairment:
//   --up-*  client -> room       --down-*  room -> client      (or --both-* for the two)
//   delay=MS        fixed one-way delay
//   jit=MS          jitter: exponential with this mean, added to the delay
//   loss=P          independent loss probability (0..1)
//   ge=PGB,PBG,LB   Gilbert-Elliott burst loss: P(good->bad) and P(bad->good) per packet, loss probability in the bad state
//   stall=RATE,MIN,MAX  a stall begins at RATE per second (Poisson); it holds EVERY packet of that direction for MIN..MAX ms
//                   (uniform) and releases them, in order, at its end (the 802.11 / AP / bufferbloat hold: nothing is lost)
//   fifo=0|1        1 (default): a packet never overtakes an earlier one of the same direction (a single queue, as on a WiFi link)
// All randomness comes from --seed.  Statistics go to stderr every 10 s and at exit (SIGTERM / SIGINT).
// Example:  gate --room room:24872 --client 30001 --client 30002 --seed 7 --both-delay 2 --both-jit 2.5 --both-loss 0.005
#include <arpa/inet.h>
#include <time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <random>
#include <string>
#include <vector>

namespace {

using ns_t = int64_t;
ns_t NowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<ns_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

volatile std::sig_atomic_t g_stop = 0;
void OnSignal(int) { g_stop = 1; }

struct Impair {
    double delay_ms = 0, jit_ms = 0, loss = 0;
    double ge_gb = 0, ge_bg = 0, ge_loss = 0;
    double stall_rate = 0, stall_min = 0, stall_max = 0;
    double ack_delay_ms = 0; // extra delay of the small datagrams (<= 24 bytes: ENet acknowledgements), as if the client held its acknowledgements
    bool fifo = true;
};

struct Dir {
    Impair cfg;
    bool ge_bad = false;
    ns_t stall_until = 0;     // while now < stall_until packets are held
    ns_t next_stall_check = 0;
    ns_t last_release = 0;    // FIFO floor
    uint64_t in = 0, dropped = 0, out = 0, bytes = 0, stalled = 0;
    double max_delay_ms = 0;
};

struct Pkt {
    ns_t release;
    uint64_t seq;
    int client;
    bool up;
    std::vector<uint8_t> data;
    bool operator>(const Pkt& o) const { return release != o.release ? release > o.release : seq > o.seq; }
};

struct Client {
    int listen_port = 0;
    int sock_c = -1, sock_r = -1;
    sockaddr_in addr{};      // the client's address, learned from its first datagram
    bool known = false;
    Dir up, down;
};

std::mt19937_64 g_rng;
double U() { return std::uniform_real_distribution<double>(0.0, 1.0)(g_rng); }
double Exp(double mean) { return mean <= 0 ? 0.0 : -mean * std::log(1.0 - U()); }

bool ParseList(const std::string& s, double* out, int n) {
    int k = 0;
    size_t i = 0;
    while (k < n && i <= s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        out[k++] = std::atof(s.substr(i, j - i).c_str());
        i = j + 1;
    }
    return k == n;
}

} // namespace

int main(int argc, char** argv) {
    std::string room_host = "127.0.0.1";
    int room_port = 24872;
    std::vector<Client> clients;
    Impair up, down;
    uint64_t seed = 1;
    auto setp = [&](const std::string& key, const std::string& val, Impair* a, Impair* b) {
        for (Impair* p : {a, b}) {
            if (!p) continue;
            if (key == "delay") p->delay_ms = std::atof(val.c_str());
            else if (key == "jit") p->jit_ms = std::atof(val.c_str());
            else if (key == "loss") p->loss = std::atof(val.c_str());
            else if (key == "fifo") p->fifo = std::atoi(val.c_str()) != 0;
            else if (key == "ackdelay") p->ack_delay_ms = std::atof(val.c_str());
            else if (key == "ge") { double v[3]; if (ParseList(val, v, 3)) { p->ge_gb = v[0]; p->ge_bg = v[1]; p->ge_loss = v[2]; } }
            else if (key == "stall") { double v[3]; if (ParseList(val, v, 3)) { p->stall_rate = v[0]; p->stall_min = v[1]; p->stall_max = v[2]; } }
        }
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--room") {
            std::string v = next();
            size_t c = v.rfind(':');
            room_host = v.substr(0, c);
            room_port = std::atoi(v.substr(c + 1).c_str());
        } else if (a == "--client") {
            Client c;
            c.listen_port = std::atoi(next().c_str());
            clients.push_back(c);
        } else if (a == "--seed") {
            seed = std::strtoull(next().c_str(), nullptr, 0);
        } else if (a.rfind("--up-", 0) == 0) {
            setp(a.substr(5), next(), &up, nullptr);
        } else if (a.rfind("--down-", 0) == 0) {
            setp(a.substr(7), next(), &down, nullptr);
        } else if (a.rfind("--both-", 0) == 0) {
            setp(a.substr(7), next(), &up, &down);
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (clients.empty()) {
        std::fprintf(stderr, "no --client\n");
        return 2;
    }
    g_rng.seed(seed);
    std::signal(SIGTERM, OnSignal);
    std::signal(SIGINT, OnSignal);

    hostent* he = gethostbyname(room_host.c_str());
    if (!he) {
        std::fprintf(stderr, "cannot resolve %s\n", room_host.c_str());
        return 2;
    }
    sockaddr_in room{};
    room.sin_family = AF_INET;
    room.sin_port = htons(room_port);
    std::memcpy(&room.sin_addr, he->h_addr_list[0], 4);

    std::vector<pollfd> pfds;
    for (Client& c : clients) {
        c.up.cfg = up;
        c.down.cfg = down;
        c.sock_c = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = INADDR_ANY;
        a.sin_port = htons(c.listen_port);
        int big = 4 << 20;
        setsockopt(c.sock_c, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
        if (bind(c.sock_c, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            std::perror("bind");
            return 1;
        }
        c.sock_r = socket(AF_INET, SOCK_DGRAM, 0);
        setsockopt(c.sock_r, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
        if (connect(c.sock_r, reinterpret_cast<sockaddr*>(&room), sizeof(room)) != 0) {
            std::perror("connect");
            return 1;
        }
    }
    for (Client& c : clients) {
        pfds.push_back({c.sock_c, POLLIN, 0});
        pfds.push_back({c.sock_r, POLLIN, 0});
    }
    std::priority_queue<Pkt, std::vector<Pkt>, std::greater<Pkt>> heap;
    uint64_t seq = 0;
    ns_t last_report = NowNs();
    std::vector<uint8_t> buf(65536);

    auto handle = [&](int ci, bool is_up, const uint8_t* data, size_t n, ns_t now) {
        Client& c = clients[ci];
        Dir& d = is_up ? c.up : c.down;
        const Impair& m = d.cfg;
        ++d.in;
        d.bytes += n;
        // stall process (Poisson arrivals, uniform length), advanced lazily up to this packet
        if (m.stall_rate > 0) {
            if (d.next_stall_check == 0) d.next_stall_check = now + static_cast<ns_t>(Exp(1.0 / m.stall_rate) * 1e9);
            while (d.next_stall_check <= now) {
                ns_t len = static_cast<ns_t>((m.stall_min + U() * (m.stall_max - m.stall_min)) * 1e6);
                d.stall_until = std::max(d.stall_until, d.next_stall_check + len);
                d.next_stall_check += len + static_cast<ns_t>(Exp(1.0 / m.stall_rate) * 1e9);
            }
        }
        // loss
        bool lose = false;
        if (m.ge_gb > 0 || m.ge_loss > 0) {
            if (d.ge_bad) { if (U() < m.ge_bg) d.ge_bad = false; } else { if (U() < m.ge_gb) d.ge_bad = true; }
            if (d.ge_bad && U() < m.ge_loss) lose = true;
        }
        if (!lose && m.loss > 0 && U() < m.loss) lose = true;
        if (lose) { ++d.dropped; return; }
        ns_t rel = now + static_cast<ns_t>((m.delay_ms + Exp(m.jit_ms)) * 1e6);
        if (now < d.stall_until) { rel = std::max(rel, d.stall_until); ++d.stalled; }
        if (m.fifo) { rel = std::max(rel, d.last_release); d.last_release = rel; }
        if (m.ack_delay_ms > 0 && n <= 24) rel += static_cast<ns_t>(m.ack_delay_ms * 1e6); // (held outside the FIFO: an ack held back does not hold the data)
        double dm = (rel - now) / 1e6;
        if (dm > d.max_delay_ms) d.max_delay_ms = dm;
        Pkt p;
        p.release = rel; p.seq = ++seq; p.client = ci; p.up = is_up; p.data.assign(data, data + n);
        heap.push(std::move(p));
    };

    auto release_due = [&](ns_t now) {
        while (!heap.empty() && heap.top().release <= now) {
            Pkt p = std::move(const_cast<Pkt&>(heap.top()));
            heap.pop();
            Client& c = clients[p.client];
            if (p.up) {
                send(c.sock_r, p.data.data(), p.data.size(), 0);
                ++c.up.out;
            } else if (c.known) {
                sendto(c.sock_c, p.data.data(), p.data.size(), 0, reinterpret_cast<sockaddr*>(&c.addr), sizeof(c.addr));
                ++c.down.out;
            }
        }
    };

    while (!g_stop) {
        ns_t now = NowNs();
        release_due(now);
        int timeout_ms = 50;
        if (!heap.empty()) {
            ns_t wait = heap.top().release - now;
            timeout_ms = wait <= 0 ? 0 : static_cast<int>((wait + 999999) / 1000000);
            if (timeout_ms > 50) timeout_ms = 50;
        }
        // sub-millisecond accuracy: ppoll with a nanosecond timeout
        timespec ts;
        ns_t wait_ns = heap.empty() ? 50000000LL : std::max<ns_t>(0, std::min<ns_t>(heap.top().release - now, 50000000LL));
        ts.tv_sec = wait_ns / 1000000000LL;
        ts.tv_nsec = wait_ns % 1000000000LL;
        int r = ppoll(pfds.data(), pfds.size(), &ts, nullptr);
        now = NowNs();
        if (r > 0) {
            for (size_t k = 0; k < clients.size(); ++k) {
                Client& c = clients[k];
                if (pfds[2 * k].revents & POLLIN) {
                    for (;;) {
                        sockaddr_in from{};
                        socklen_t fl = sizeof(from);
                        ssize_t n = recvfrom(c.sock_c, buf.data(), buf.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from), &fl);
                        if (n < 0) break;
                        c.addr = from; c.known = true;
                        handle(static_cast<int>(k), true, buf.data(), n, now);
                    }
                }
                if (pfds[2 * k + 1].revents & POLLIN) {
                    for (;;) {
                        ssize_t n = recv(c.sock_r, buf.data(), buf.size(), MSG_DONTWAIT);
                        if (n < 0) break;
                        handle(static_cast<int>(k), false, buf.data(), n, now);
                    }
                }
            }
        }
        release_due(NowNs());
        if (now - last_report > 10000000000LL) {
            last_report = now;
            for (size_t k = 0; k < clients.size(); ++k) {
                const Client& c = clients[k];
                std::fprintf(stderr, "gate c%zu up in %llu drop %llu out %llu | down in %llu drop %llu out %llu\n", k,
                             (unsigned long long)c.up.in, (unsigned long long)c.up.dropped, (unsigned long long)c.up.out,
                             (unsigned long long)c.down.in, (unsigned long long)c.down.dropped, (unsigned long long)c.down.out);
            }
        }
    }
    for (size_t k = 0; k < clients.size(); ++k) {
        const Client& c = clients[k];
        std::fprintf(stderr,
                     "GATE c%zu up: in %llu dropped %llu out %llu bytes %llu stalled %llu maxdelay %.1f ms | down: in %llu dropped %llu out %llu bytes %llu stalled %llu maxdelay %.1f ms\n",
                     k, (unsigned long long)c.up.in, (unsigned long long)c.up.dropped, (unsigned long long)c.up.out, (unsigned long long)c.up.bytes,
                     (unsigned long long)c.up.stalled, c.up.max_delay_ms, (unsigned long long)c.down.in, (unsigned long long)c.down.dropped,
                     (unsigned long long)c.down.out, (unsigned long long)c.down.bytes, (unsigned long long)c.down.stalled, c.down.max_delay_ms);
    }
    return 0;
}
