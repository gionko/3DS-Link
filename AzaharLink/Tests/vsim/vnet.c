/* vnet.c: the console's side of the model. The libc socket calls that Sources/enet/enet_3ds.c makes (socket, sendto, recvfrom, poll, ...)
   are defined HERE, so that the plugin's own platform layer runs unchanged on a virtual wire and a virtual clock; each call costs what
   the profile says (the soc service's round trip on the console) and moves the console's clock. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <unistd.h>

#include "vsim.h"

/* ---------------------------------------------------------------- clock and numbers */
uint64_t vt_us = 0;
int vt_actor = 0;
uint64_t vsim_now_us(void) { return vt_us; }

/* three independent streams (the costs of the console's calls, the wire up, the wire down): two runs that differ in what the console does
   see the same jitter on the frames the room sends, so a difference between them is the change and not the dice */
static uint64_t rng_s[3] = {88172645463325252ull, 1234567890123456789ull, 987654321987654321ull};
static uint64_t rnd64s(int s) { uint64_t x = rng_s[s]; x ^= x << 13; x ^= x >> 7; x ^= x << 17; rng_s[s] = x; return x; }
static double rnd_us(int s) { return (double)(rnd64s(s) >> 11) / 9007199254740992.0; }
static double rnd_ns(int s) {
    double u = rnd_us(s), w = rnd_us(s);
    if (u < 1e-12) u = 1e-12;
    return sqrt(-2.0 * log(u)) * cos(6.283185307179586 * w);
}
static double rnd_logns(int s, double mean, double cv) {
    if (mean <= 0) return 0;
    const double s2 = log(1.0 + cv * cv), mu = log(mean) - 0.5 * s2;
    return exp(mu + sqrt(s2) * rnd_ns(s));
}
double rnd_u(void) { return rnd_us(0); }
void rnd_seed(uint64_t s) {
    rng_s[0] = (s ? s : 88172645463325252ull) * 6364136223846793005ull + 1442695040888963407ull;
    rng_s[1] = (s ? s : 1ull) * 2862933555777941757ull + 3037000493ull;
    rng_s[2] = (s ? s : 2ull) * 3202034522624059733ull + 1013904223ull;
    for (int i = 0; i < 3; ++i) for (int k = 0; k < 16; ++k) (void)rnd64s(i);
}
double rnd_norm(void) { return rnd_ns(0); }
double rnd_logn(double mean, double cv) { return rnd_logns(0, mean, cv); }

Profile g_prof;
NetCfg g_net;
uint64_t g_outage_from_us = 0, g_outage_to_us = 0;
int g_outage_silent = 0;
uint32_t g_drop_connects = 0;
NetStats g_ns;

uint64_t t_inject[VS_MAX_FRAMES], t_arrive[VS_MAX_FRAMES], t_deliver[VS_MAX_FRAMES];
uint32_t g_frames_injected = 0;
uint8_t g_sent_cnt[VS_MAX_FRAMES], g_lost_cnt[VS_MAX_FRAMES];

/* ---------------------------------------------------------------- the wire */
typedef struct Dgram {
    uint64_t arr_us;
    uint32_t len;
    uint8_t data[1536];
} Dgram;
typedef struct {
    Dgram* q;
    size_t head, tail, cap;
    uint64_t last_arr;
} Queue;
static Queue to_client, to_room;

static void q_push(Queue* q, const Dgram* d) {
    if (q->tail == q->cap) {
        if (q->head > 0 && q->head >= q->cap / 2) {
            memmove(q->q, q->q + q->head, (q->tail - q->head) * sizeof(Dgram));
            q->tail -= q->head;
            q->head = 0;
        } else {
            q->cap = q->cap ? q->cap * 2 : 4096;
            q->q = (Dgram*)realloc(q->q, q->cap * sizeof(Dgram));
        }
    }
    q->q[q->tail++] = *d;
}
static int q_empty(const Queue* q) { return q->head == q->tail; }
uint64_t vnet_client_next_arrival(void) { return q_empty(&to_client) ? UINT64_MAX : to_client.q[to_client.head].arr_us; }
uint32_t vnet_client_backlog(uint64_t now) {
    uint32_t n = 0;
    for (size_t i = to_client.head; i < to_client.tail && to_client.q[i].arr_us <= now; ++i) ++n;
    return n;
}
uint64_t vnet_room_next_arrival(void) { return q_empty(&to_room) ? UINT64_MAX : to_room.q[to_room.head].arr_us; }

static int client_down(uint64_t at) { return g_outage_to_us > g_outage_from_us && at >= g_outage_from_us && at < g_outage_to_us; }
/* the console's calls fail only in an outage that is not silent (a WiFi that is away and a socket that says so: ENETDOWN) */
static int client_err(uint64_t at) { return client_down(at) && !g_outage_silent; }

/* when a datagram sent at `at` arrives (a FIFO wire: no overtaking); 0 = lost */
static int wire(Queue* q, uint64_t at, double base_us, double sd_us, Dgram* d) {
    const int st = q == &to_room ? 1 : 2;
    if (g_net.loss > 0 && rnd_us(st) < g_net.loss) return 0;
    const double j = sd_us > 0 ? rnd_logns(st, sd_us, 1.0) - sd_us : 0.0; /* a skewed jitter of mean 0: mostly small, sometimes large */
    double delay = base_us + j;
    if (delay < base_us * 0.5) delay = base_us * 0.5;
    uint64_t arr = at + (uint64_t)delay;
    if (arr < q->last_arr) arr = q->last_arr;
    q->last_arr = arr;
    d->arr_us = arr;
    return 1;
}

/* the frame ids in a datagram of the room (SEND_RELIABLE commands whose data starts with a 32-bit id) */
extern size_t enet_protocol_command_size(uint8_t);
static void walk(const uint8_t* p, size_t n, uint64_t arr_us, int* acks, int* other, uint8_t* cnt) {
    if (n < 4) return;
    const uint16_t peer_id = (uint16_t)((p[0] << 8) | p[1]);
    size_t i = (peer_id & 0x8000) ? 4 : 2;
    while (i + 4 <= n) {
        const uint8_t cmd = p[i] & 0x0F;
        if (cmd == 0 || cmd > 12) break;
        const size_t sz = enet_protocol_command_size(cmd);
        if (!sz || i + sz > n) break;
        size_t dl = 0;
        if (cmd == 6) dl = (size_t)((p[i + 4] << 8) | p[i + 5]);
        else if (cmd == 7 || cmd == 9 || cmd == 8 || cmd == 12) dl = (size_t)((p[i + 6] << 8) | p[i + 7]);
        if (acks && other) {
            if (cmd == 1) ++*acks; else ++*other;
        }
        if ((arr_us || cnt) && cmd == 6 && dl >= 4 && i + sz + 4 <= n) {
            uint32_t id;
            memcpy(&id, p + i + sz, 4);
            if (id < VS_MAX_FRAMES) {
                if (arr_us && t_arrive[id] == 0) t_arrive[id] = arr_us;
                if (cnt && cnt[id] < 255) ++cnt[id];
            }
        }
        i += sz + dl;
    }
}

int vnet_room_send(const uint8_t* buf, size_t len, uint64_t now_us) {
    Dgram d;
    if (len > sizeof(d.data)) return -1;
    memcpy(d.data, buf, len);
    d.len = (uint32_t)len;
    walk(d.data, d.len, 0, NULL, NULL, g_sent_cnt);        /* (a frame that goes out a second time is a retransmission) */
    if (client_down(now_us)) { walk(d.data, d.len, 0, NULL, NULL, g_lost_cnt); return (int)len; }  /* nobody hears it */
    if (!wire(&to_client, now_us, g_net.down_base_us, g_net.down_sd_us, &d)) { walk(d.data, d.len, 0, NULL, NULL, g_lost_cnt); return (int)len; }
    if (client_down(d.arr_us)) { walk(d.data, d.len, 0, NULL, NULL, g_lost_cnt); return (int)len; }
    walk(d.data, d.len, d.arr_us, NULL, NULL, NULL);
    q_push(&to_client, &d);
    return (int)len;
}
int vnet_room_recv(uint8_t* buf, size_t cap, uint64_t now_us) {
    if (q_empty(&to_room) || to_room.q[to_room.head].arr_us > now_us) return 0;
    const Dgram* d = &to_room.q[to_room.head++];
    const size_t n = d->len < cap ? d->len : cap;
    memcpy(buf, d->data, n);
    return (int)n;
}

/* ---------------------------------------------------------------- the console's libc */
#define VFD 1001

static double cost(double mean) { return rnd_logn(mean, g_prof.cv); }
static double slow(int kind) { /* the thread was not scheduled for a while */
    const double p100 = g_prof.s100[kind] >= 0 ? g_prof.s100[kind] : g_prof.slow100_p;
    const double p20 = g_prof.s20[kind] >= 0 ? g_prof.s20[kind] : g_prof.slow20_p;
    const double u = rnd_u();
    const double scale = g_prof.slow_scale_ms > 0 ? g_prof.slow_scale_ms : 15.0;
    double ms = 0;
    if (u < p100) ms = 100.0 - scale * log(1.0 - rnd_u());
    else if (u < p100 + p20) ms = 20.0 - scale * log(1.0 - rnd_u()); /* 20 ms and an exponential tail: most stalls are short, a few are long */
    if (ms > g_prof.slow_max_ms) ms = g_prof.slow_max_ms;
    return ms * 1000.0;
}
static void spend(int kind, double us) {
    g_ns.n[kind]++;
    g_ns.us[kind] += us;
    vt_us += (uint64_t)us;
}

/* the plugin's own counters of the soc calls (Sources/azl/log.cpp): not needed here */
void azl_c_soc_call(int kind, unsigned long long ticks) { (void)kind; (void)ticks; }
void azl_c_room_datagram(unsigned bytes, unsigned long long gap) { (void)bytes; (void)gap; }
void azl_c_rcvbuf_result(int asked, int got) { (void)asked; (void)got; }
void azl_c_log(const char* fmt, ...) { (void)fmt; }

int socket(int domain, int type, int protocol) { (void)domain; (void)type; (void)protocol; return VFD; }
int bind(int fd, const struct sockaddr* a, socklen_t l) { (void)fd; (void)a; (void)l; return 0; }
int getsockname(int fd, struct sockaddr* a, socklen_t* l) {
    (void)fd;
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(40001);
    memcpy(a, &sin, *l < sizeof sin ? *l : sizeof sin);
    *l = sizeof sin;
    return 0;
}
int listen(int fd, int b) { (void)fd; (void)b; return 0; }
int connect(int fd, const struct sockaddr* a, socklen_t l) { (void)fd; (void)a; (void)l; return 0; }
int accept(int fd, struct sockaddr* a, socklen_t* l) { (void)fd; (void)a; (void)l; errno = EINVAL; return -1; }
int shutdown(int fd, int how) { (void)fd; (void)how; return 0; }
int setsockopt(int fd, int lvl, int opt, const void* v, socklen_t l) { (void)fd; (void)lvl; (void)opt; (void)v; (void)l; return 0; }
int getsockopt(int fd, int lvl, int opt, void* v, socklen_t* l) {
    (void)fd; (void)lvl;
    if (opt == SO_RCVBUF && *l >= sizeof(int)) { *(int*)v = 65536; *l = sizeof(int); return 0; }
    if (*l >= sizeof(int)) { *(int*)v = 0; *l = sizeof(int); }
    return 0;
}
int fcntl(int fd, int cmd, ...) { (void)fd; (void)cmd; return 0; }
int close(int fd) { if (fd == VFD) return 0; return (int)syscall(SYS_close, fd); }

ssize_t sendto(int fd, const void* buf, size_t len, int flags, const struct sockaddr* to, socklen_t tolen) {
    (void)fd; (void)flags; (void)to; (void)tolen;
    if (client_err(vt_us)) {
        spend(SOC_SEND, cost(g_prof.send_us * 0.25));
        errno = ENETDOWN;
        return -1;
    }
    spend(SOC_SEND, cost(g_prof.send_us) + slow(SOC_SEND));
    ++g_ns.dgrams_out;
    {
        int acks = 0, other = 0;
        walk((const uint8_t*)buf, len, 0, &acks, &other, NULL);
        if (acks && !other) ++g_ns.ack_only_out; else if (acks) ++g_ns.ack_data_out; else ++g_ns.data_only_out;
    }
    Dgram d;
    if (len > sizeof d.data) { errno = EMSGSIZE; return -1; }
    memcpy(d.data, buf, len);
    d.len = (uint32_t)len;
    if (g_drop_connects && len >= 3) {
        const uint8_t* b = (const uint8_t*)buf;
        const uint16_t pid = (uint16_t)((b[0] << 8) | b[1]);
        const size_t h = (pid & 0x8000) ? 4 : 2;
        if (len > h && (b[h] & 0x0F) == 2) { --g_drop_connects; return (ssize_t)len; }
    }
    if (client_down(vt_us)) return (ssize_t)len;
    if (wire(&to_room, vt_us, g_net.up_base_us, g_net.up_sd_us, &d)) q_push(&to_room, &d);
    return (ssize_t)len;
}
ssize_t send(int fd, const void* buf, size_t len, int flags) { return sendto(fd, buf, len, flags, NULL, 0); }

ssize_t recvfrom(int fd, void* buf, size_t len, int flags, struct sockaddr* from, socklen_t* fromlen) {
    (void)fd; (void)flags;
    if (client_err(vt_us)) {
        spend(SOC_RECVE, cost(g_prof.recvE_us * 0.25));
        errno = ENETDOWN;
        return -1;
    }
    vsim_run_room_until(vt_us);
    if (q_empty(&to_client) || to_client.q[to_client.head].arr_us > vt_us) {
        spend(SOC_RECVE, cost(g_prof.recvE_us) + slow(SOC_RECVE));
        errno = EWOULDBLOCK;
        return -1;
    }
    const Dgram* d = &to_client.q[to_client.head++];
    const size_t n = d->len < len ? d->len : len;
    memcpy(buf, d->data, n);
    spend(SOC_RECV, cost(g_prof.recv_us) + slow(SOC_RECV));
    ++g_ns.dgrams_in;
    if (from && fromlen && *fromlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof sin);
        sin.sin_family = AF_INET;
        sin.sin_port = htons(24872);
        sin.sin_addr.s_addr = htonl(0x0A000001);
        memcpy(from, &sin, sizeof sin);
        *fromlen = sizeof sin;
    }
    return (ssize_t)n;
}

int poll(struct pollfd* fds, nfds_t nfds, int timeout_ms) {
    (void)nfds;
    fds[0].revents = 0;
    if (client_err(vt_us)) {
        spend(SOC_POLLR, cost(g_prof.pollr_us * 0.25));
        errno = ENETDOWN;
        return -1;
    }
    vsim_run_room_until(vt_us);
    if (vnet_client_next_arrival() <= vt_us) {
        spend(SOC_POLLR, cost(g_prof.pollr_us) + slow(SOC_POLLR));
        fds[0].revents = POLLIN;
        return 1;
    }
    const uint64_t t0 = vt_us;
    const uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : t0 + (uint64_t)timeout_ms * 1000ull;
    for (;;) {
        const uint64_t a = vnet_client_next_arrival();
        const uint64_t r = vsim_next_room_event_us();
        const uint64_t wake = a < deadline ? a : deadline;
        if (r < wake) { /* the room does something first: it may send us something sooner */
            vsim_run_room_until(r);
            continue;
        }
        if (wake == UINT64_MAX) return 0;
        const int got = a <= deadline;
        vt_us = wake;
        if (!got) {
            g_ns.n[SOC_POLLW]++;
            g_ns.us[SOC_POLLW] += (double)(vt_us - t0);
            if (client_err(vt_us)) { errno = ENETDOWN; return -1; }
            return 0;
        }
        const double wk = cost(g_prof.wake_us) + slow(SOC_POLLW);
        g_ns.n[SOC_POLLW]++;
        g_ns.us[SOC_POLLW] += (double)(vt_us - t0) + wk;
        vt_us += (uint64_t)wk;
        vsim_run_room_until(vt_us);
        if (client_err(vt_us)) { errno = ENETDOWN; return -1; }
        fds[0].revents = POLLIN;
        return 1;
    }
}
