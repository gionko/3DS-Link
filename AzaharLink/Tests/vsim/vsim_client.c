/* vsim_client.c: the loop of the plugin's network thread (RoomClient::Run, room_client.cpp) on the virtual clock, driving the plugin's
   own ENet (whichever tree this is built against) against the room (room_side.c, ENet 1.3.18).

   Input : a configuration file (key=value) and a trace: lines "R <t_us> <size>" = the room hands the console a frame at that time (a
           frame of another member, a beacon), "T <t_us> <size> <reliable>" = the game queues a frame (SendTo) at that time.
   Output: key=value lines on stdout, and with "dump=<file>" a CSV of every frame of the room (inject, arrive, deliver in microseconds). */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <enet/enet.h>

#include "vsim.h"

/* ---------------------------------------------------------------- configuration */
typedef struct {
    char name[64];
    char dump[256];
    double seconds;
    uint64_t seed;
    double connect_rtt;       /* 0: leave ENet's 500 ms */
    uint32_t drop_connects;
    double outage_from_s, outage_to_s;
    int ping_recovery;
    int outage_silent, probe;
    double hard_stop_s;
    /* knobs of the plugin's ENet (the patched tree): -1 = leave the default */
    int skip_empty, max_dgrams, ack_hold_events, ack_hold_ms;
    int debatch;
} Cfg;
static Cfg cfg = {"run", "", 60, 1, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1, 0};

static void cfg_set(const char* k, const char* v) {
    const double d = atof(v);
    if (!strcmp(k, "name")) snprintf(cfg.name, sizeof cfg.name, "%s", v);
    else if (!strcmp(k, "dump")) snprintf(cfg.dump, sizeof cfg.dump, "%s", v);
    else if (!strcmp(k, "seconds")) cfg.seconds = d;
    else if (!strcmp(k, "seed")) cfg.seed = (uint64_t)d;
    else if (!strcmp(k, "connect_rtt")) cfg.connect_rtt = d;
    else if (!strcmp(k, "drop_connects")) cfg.drop_connects = (uint32_t)d;
    else if (!strcmp(k, "outage_from_s")) cfg.outage_from_s = d;
    else if (!strcmp(k, "outage_to_s")) cfg.outage_to_s = d;
    else if (!strcmp(k, "ping_recovery")) cfg.ping_recovery = (int)d;
    else if (!strcmp(k, "outage_silent")) cfg.outage_silent = (int)d;
    else if (!strcmp(k, "probe")) cfg.probe = (int)d;
    else if (!strcmp(k, "hard_stop_s")) cfg.hard_stop_s = d;
    else if (!strcmp(k, "skip_empty")) cfg.skip_empty = (int)d;
    else if (!strcmp(k, "max_dgrams")) cfg.max_dgrams = (int)d;
    else if (!strcmp(k, "ack_hold_events")) cfg.ack_hold_events = (int)d;
    else if (!strcmp(k, "ack_hold_ms")) cfg.ack_hold_ms = (int)d;
    else if (!strcmp(k, "recv_us")) g_prof.recv_us = d;
    else if (!strcmp(k, "recvE_us")) g_prof.recvE_us = d;
    else if (!strcmp(k, "send_us")) g_prof.send_us = d;
    else if (!strcmp(k, "pollr_us")) g_prof.pollr_us = d;
    else if (!strcmp(k, "wake_us")) g_prof.wake_us = d;
    else if (!strcmp(k, "cv")) g_prof.cv = d;
    else if (!strcmp(k, "slow20_p")) g_prof.slow20_p = d;
    else if (!strcmp(k, "slow100_p")) g_prof.slow100_p = d;
    else if (!strcmp(k, "slow_max_ms")) g_prof.slow_max_ms = d;
    else if (!strcmp(k, "slow_scale_ms")) g_prof.slow_scale_ms = d;
    else if (!strcmp(k, "slow20_recv")) g_prof.s20[SOC_RECV] = g_prof.s20[SOC_RECVE] = d;
    else if (!strcmp(k, "slow100_recv")) g_prof.s100[SOC_RECV] = g_prof.s100[SOC_RECVE] = d;
    else if (!strcmp(k, "slow20_send")) g_prof.s20[SOC_SEND] = d;
    else if (!strcmp(k, "slow100_send")) g_prof.s100[SOC_SEND] = d;
    else if (!strcmp(k, "slow20_wake")) g_prof.s20[SOC_POLLR] = g_prof.s20[SOC_POLLW] = d;
    else if (!strcmp(k, "slow100_wake")) g_prof.s100[SOC_POLLR] = g_prof.s100[SOC_POLLW] = d;
    else if (!strcmp(k, "handler_us")) g_prof.handler_us = d;
    else if (!strcmp(k, "round_us")) g_prof.round_us = d;
    else if (!strcmp(k, "up_base_us")) g_net.up_base_us = d;
    else if (!strcmp(k, "up_sd_us")) g_net.up_sd_us = d;
    else if (!strcmp(k, "down_base_us")) g_net.down_base_us = d;
    else if (!strcmp(k, "down_sd_us")) g_net.down_sd_us = d;
    else if (!strcmp(k, "loss")) g_net.loss = d;
    else fprintf(stderr, "vsim: unknown key %s\n", k);
}

static void load_cfg(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { perror(path); exit(2); }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char* v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        cfg_set(line, v);
    }
    fclose(f);
}

/* ---------------------------------------------------------------- the trace */
typedef struct { uint64_t t_us; uint32_t size, id; } InjEv;
typedef struct { uint64_t t_us; uint32_t size; int rel; uint32_t id; } TxEv;
static InjEv* inj; static size_t n_inj, inj_next;
static TxEv* txe; static size_t n_tx, tx_next;
static uint64_t t_base_us = 3000000; /* the traffic starts 3 s into the run: the connection is up by then */

static void load_trace(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { perror(path); exit(2); }
    size_t cap_i = 1 << 16, cap_t = 1 << 16;
    inj = (InjEv*)malloc(cap_i * sizeof(InjEv));
    txe = (TxEv*)malloc(cap_t * sizeof(TxEv));
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char kind;
        double t; unsigned size; int rel = 0;
        if (line[0] == 'R' && sscanf(line, "%c %lf %u", &kind, &t, &size) == 3) {
            if (n_inj == cap_i) { cap_i *= 2; inj = (InjEv*)realloc(inj, cap_i * sizeof(InjEv)); }
            inj[n_inj].t_us = t_base_us + (uint64_t)t; inj[n_inj].size = size; inj[n_inj].id = (uint32_t)n_inj; ++n_inj;
        } else if (line[0] == 'T' && sscanf(line, "%c %lf %u %d", &kind, &t, &size, &rel) >= 3) {
            if (n_tx == cap_t) { cap_t *= 2; txe = (TxEv*)realloc(txe, cap_t * sizeof(TxEv)); }
            txe[n_tx].t_us = t_base_us + (uint64_t)t; txe[n_tx].size = size; txe[n_tx].rel = rel; txe[n_tx].id = 0x80000000u | (uint32_t)n_tx; ++n_tx;
        }
    }
    fclose(f);
    if (n_inj >= VS_MAX_FRAMES) { fprintf(stderr, "vsim: trace too long\n"); exit(2); }
}

/* ---------------------------------------------------------------- the room's events in time order */
static uint64_t room_last_run = 0;
uint64_t vsim_next_room_event_us(void) {
    uint64_t t = room_last_run + 16000ull; /* ServerLoop: enet_host_service(server, &event, 16) */
    if (inj_next < n_inj && inj[inj_next].t_us < t) t = inj[inj_next].t_us;
    const uint64_t a = vnet_room_next_arrival();
    if (a < t) t = a;
    return t;
}
void vsim_run_room_until(uint64_t T) {
    for (;;) {
        const uint64_t t = vsim_next_room_event_us();
        if (t > T) break;
        room_set_now(t);
        while (inj_next < n_inj && inj[inj_next].t_us <= t) {
            t_inject[inj[inj_next].id] = inj[inj_next].t_us;
            room_inject(inj[inj_next].id, inj[inj_next].size);
            ++g_frames_injected;
            ++inj_next;
        }
        room_service();
        room_last_run = t;
    }
}

/* ---------------------------------------------------------------- the loop of the network thread */
static uint32_t service_timeout_for(int queued, uint32_t now_ms, uint32_t last_enqueue_ms) {
    if (queued) return 0;
    return now_ms - last_enqueue_ms < 10 ? 1 : 3; /* kQuickPollWindowMs, kQuickPollMs, kServiceTimeoutMs */
}
static uint32_t error_pause_ms(uint32_t n) { return n == 0 ? 0u : n >= 7 ? 128u : (2u << (n - 1)); }

static int cmp_u64(const void* a, const void* b) { const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b; return x < y ? -1 : x > y; }
static void pct(const char* name, uint64_t* v, size_t n) {
    if (!n) { printf("%s_n=0\n", name); return; }
    qsort(v, n, sizeof *v, cmp_u64);
    printf("%s_n=%zu\n%s_p50=%.3f\n%s_p90=%.3f\n%s_p95=%.3f\n%s_p99=%.3f\n%s_p999=%.3f\n%s_max=%.3f\n%s_mean=", name, n, name, v[n * 50 / 100] / 1000.0, name, v[n * 90 / 100] / 1000.0,
           name, v[n * 95 / 100] / 1000.0, name, v[n * 99 / 100] / 1000.0, name, v[(size_t)((double)n * 0.999)] / 1000.0, name, v[n - 1] / 1000.0, name);
    double s = 0;
    for (size_t i = 0; i < n; ++i) s += (double)v[i];
    printf("%.3f\n", s / (double)n / 1000.0);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: vsim <config> <trace>\n"); return 2; }
    for (int k = 0; k < SOC_KINDS_N; ++k) g_prof.s20[k] = g_prof.s100[k] = -1.0;
    load_cfg(argv[1]);
    load_trace(argv[2]);
    rnd_seed(cfg.seed);
    vt_us = 1000000;
    room_set_now(vt_us);
    room_last_run = vt_us;
    room_init();
    if (cfg.drop_connects) g_drop_connects = cfg.drop_connects;
    if (cfg.outage_to_s > cfg.outage_from_s) {
        g_outage_from_us = t_base_us + (uint64_t)(cfg.outage_from_s * 1e6);
        g_outage_to_us = t_base_us + (uint64_t)(cfg.outage_to_s * 1e6);
        g_outage_silent = cfg.outage_silent;
    }

    enet_initialize();
    ENetHost* client = enet_host_create(NULL, 1, 1, 0, 0);
    client->maximumPacketSize = 64 * 1024;
    client->maximumWaitingData = 192 * 1024;
#ifdef VSIM_KNOBS
    if (cfg.skip_empty >= 0) client->azlSkipEmptyRecv = (enet_uint32)cfg.skip_empty;
    if (cfg.max_dgrams >= 0) client->azlMaxDatagrams = (enet_uint32)cfg.max_dgrams;
    if (cfg.ack_hold_events >= 0) client->azlAckHoldEvents = (enet_uint32)cfg.ack_hold_events;
    if (cfg.ack_hold_ms >= 0) client->azlAckHoldMs = (enet_uint32)cfg.ack_hold_ms;
#endif
    ENetAddress addr;
    enet_address_set_host_ip(&addr, "10.0.0.1");
    addr.port = 24872;
    ENetPeer* server = enet_host_connect(client, &addr, 1, 0);
    if (cfg.connect_rtt > 0) { server->roundTripTime = (enet_uint32)cfg.connect_rtt; server->roundTripTimeVariance = 0; }
    const uint64_t connect_began = vt_us;
    int connected = 0;
    for (const uint64_t until = vt_us + 5000000ull; vt_us < until && !connected;) {
        ENetEvent ev;
        const int r = enet_host_service(client, &ev, 100);
        if (r > 0 && ev.type == ENET_EVENT_TYPE_CONNECT) connected = 1;
    }
    printf("connect_ms=%.1f\n", connected ? (vt_us - connect_began) / 1000.0 : -1.0);
    if (!connected) { printf("connected=0\n"); return 1; }
    enet_peer_throttle_configure(server, ENET_PEER_PACKET_THROTTLE_INTERVAL, ENET_PEER_PACKET_THROTTLE_SCALE, 0);
    if (vt_us < t_base_us) { /* the rest of the quiet time before the traffic: the loop at rest */
        while (vt_us < t_base_us) {
            ENetEvent ev;
            enet_host_service(client, &ev, 3);
            vt_us += 20;
        }
    }

    const uint64_t t_end = t_base_us + (uint64_t)(cfg.seconds * 1e6) + 1000000ull;
    uint32_t errors_in_a_row = 0, last_enqueue_ms = 0;
    uint64_t lost_at = 0, rounds = 0, polls_now = 0, polls_quick = 0, tx_sent = 0, tx_delay_n = 0;
    double tx_delay_sum = 0, tx_delay_max = 0;
    double handler_total_us = 0, round_total_us = 0;
    uint64_t svc_max_us = 0, round_max_us = 0, last_round_start = vt_us, loop_gap_max_us = 0, pings_sent = 0, recoveries = 0;
    uint64_t* tx_wait = (uint64_t*)malloc((n_tx + 1) * sizeof(uint64_t));
    size_t n_txwait = 0;
    uint64_t drained_max = 0, drained_hist_gt8 = 0;
    /* windows of 5 s, like the plugin's statistics: the longest pause between two rounds, the most datagrams one service call read, the backlog */
    enum { MAXWIN = 4096 };
    static uint64_t win_gap[MAXWIN], win_dg[MAXWIN], win_backlog[MAXWIN];
    static uint64_t win_tx_n[MAXWIN];
    uint64_t win_start = vt_us;
    unsigned nwin = 0;
    uint64_t wgap = 0, wdg = 0, wbl = 0;
    uint32_t recv_before = 0;
    (void)recv_before;
    int done = 0;
    uint32_t datagrams_seen = client->totalReceivedPackets;
    uint64_t last_probe_ms = 0, probes = 0;
    while (vt_us < t_end && !done) {
        const uint64_t round_start = vt_us;
        const uint64_t gap = round_start - last_round_start;
        if (gap > loop_gap_max_us) loop_gap_max_us = gap;
        last_round_start = round_start;
        ++rounds;
        if (gap > wgap) wgap = gap;
        if (gap > 40000 && getenv("VSIM_TRACE")) fprintf(stderr, "gap %.1f ms before t=%.3f s (backlog %u)\n", gap / 1000.0, (vt_us - t_base_us) / 1e6, vnet_client_backlog(vt_us));
        { const uint32_t bl = vnet_client_backlog(vt_us); if (bl > wbl) wbl = bl; }
        if (vt_us - win_start >= 5000000ull && nwin < MAXWIN) { win_gap[nwin] = wgap; win_dg[nwin] = wdg; win_backlog[nwin] = wbl; ++nwin; wgap = wdg = wbl = 0; win_start = vt_us; }
        /* the frames the game queued before this round */
        size_t queued_to = tx_next;
        while (queued_to < n_tx && txe[queued_to].t_us <= vt_us) ++queued_to;
        const int queued = queued_to > tx_next;
        if (queued) last_enqueue_ms = (uint32_t)(txe[queued_to - 1].t_us / 1000);
        const uint32_t timeout = service_timeout_for(queued, (uint32_t)(vt_us / 1000), last_enqueue_ms);
        if (timeout == 0) ++polls_now; else if (timeout == 1) ++polls_quick;
        ENetEvent ev;
        const uint64_t svc_start = vt_us;
        const uint32_t recvs_before = (uint32_t)g_ns.n[SOC_RECV];
        const int r = enet_host_service(client, &ev, timeout);
        const uint64_t svc_us = vt_us - svc_start;
        if (svc_us > svc_max_us) svc_max_us = svc_us;
        {
            const uint64_t dg = g_ns.n[SOC_RECV] - recvs_before;
            if (dg > drained_max) drained_max = dg;
            if (dg > wdg) wdg = dg;
            if (dg > 8) ++drained_hist_gt8;
        }
        if (r < 0) {
            ++errors_in_a_row;
            vt_us += (uint64_t)error_pause_ms(errors_in_a_row) * 1000ull;
        } else if (errors_in_a_row != 0) {
            errors_in_a_row = 0;
            ++recoveries;
            if (cfg.ping_recovery && server->state == ENET_PEER_STATE_CONNECTED) { enet_peer_ping(server); ++pings_sent; }
        }
        if (cfg.probe && client->totalReceivedPackets != datagrams_seen) {
            /* room_client.cpp: the room spoke while packets of ours had gone unanswered for 0.5 s: ping it (the answer restarts ENet's count of the silence) */
            datagrams_seen = client->totalReceivedPackets;
            const uint64_t now_ms = vt_us / 1000;
            if (server->earliestTimeout != 0 && server->state == ENET_PEER_STATE_CONNECTED && client->serviceTime - server->earliestTimeout >= 500 && now_ms - last_probe_ms >= 250) {
                enet_peer_ping(server);
                last_probe_ms = now_ms;
                ++probes;
            }
        }
        if (r > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                uint32_t id = 0xFFFFFFFFu;
                if (ev.packet->dataLength >= 4) memcpy(&id, ev.packet->data, 4);
                const double h = rnd_logn(g_prof.handler_us, 0.5);
                handler_total_us += h;
                vt_us += (uint64_t)h;
                if (id < VS_MAX_FRAMES) t_deliver[id] = vt_us;
                enet_packet_destroy(ev.packet);
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                lost_at = vt_us;
                done = 1;
            }
        }
        if (done) break;
        /* the game's frames into ENet, then a flush */
        int sent_any = 0;
        while (tx_next < n_tx && txe[tx_next].t_us <= vt_us) {
            const TxEv* t = &txe[tx_next++];
            uint8_t buf[1600];
            const uint32_t size = t->size < 8 ? 8 : (t->size > 1500 ? 1500 : t->size);
            memset(buf, 0x5A, size);
            memcpy(buf, &t->id, 4);
            ENetPacket* p = enet_packet_create(buf, size, t->rel ? (enet_uint32)ENET_PACKET_FLAG_RELIABLE : (enet_uint32)(ENET_PACKET_FLAG_UNSEQUENCED | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT));
            enet_peer_send(server, 0, p);
            sent_any = 1;
            ++tx_sent;
            const uint64_t w = vt_us - t->t_us;
            tx_wait[n_txwait++] = w;
            tx_delay_sum += (double)w; ++tx_delay_n;
            if ((double)w > tx_delay_max) tx_delay_max = (double)w;
        }
        if (sent_any && errors_in_a_row == 0) enet_host_flush(client);
        {
            const double c = rnd_logn(g_prof.round_us, 0.5);
            round_total_us += c;
            vt_us += (uint64_t)c;
        }
        const uint64_t round_us = vt_us - round_start;
        if (round_us > round_max_us) round_max_us = round_us;
    }

    /* ------------------------------------------------------------ results */
    const double secs = cfg.seconds;
    RoomStats rs;
    room_get_stats(&rs);
    printf("name=%s\nseconds=%.1f\nconnected=1\nframes_injected=%u\nlost_at_s=%.3f\nrounds=%llu\nrounds_s=%.1f\n", cfg.name, secs, g_frames_injected,
           lost_at ? (lost_at - t_base_us) / 1e6 : -1.0, (unsigned long long)rounds, rounds / secs);
    printf("polls_now=%llu\npolls_quick=%llu\ntx_sent=%llu\ntx_wait_mean_ms=%.3f\ntx_wait_max_ms=%.3f\n", (unsigned long long)polls_now, (unsigned long long)polls_quick,
           (unsigned long long)tx_sent, tx_delay_n ? tx_delay_sum / tx_delay_n / 1000.0 : 0.0, tx_delay_max / 1000.0);
    const char* kn[SOC_KINDS] = {"recv", "recvE", "send", "pollr", "pollw"};
    for (int k = 0; k < SOC_KINDS; ++k)
        printf("soc_%s_n=%llu\nsoc_%s_ms_s=%.2f\nsoc_%s_n_s=%.1f\n", kn[k], (unsigned long long)g_ns.n[k], kn[k], g_ns.us[k] / 1000.0 / secs, kn[k], g_ns.n[k] / secs);
    printf("handler_ms_s=%.2f\nround_ms_s=%.2f\n", handler_total_us / 1000.0 / secs, round_total_us / 1000.0 / secs);
    printf("busy_ms_s=%.2f\n", (g_ns.us[SOC_RECV] + g_ns.us[SOC_RECVE] + g_ns.us[SOC_SEND] + g_ns.us[SOC_POLLR] + handler_total_us + round_total_us) / 1000.0 / secs);
    printf("dgrams_out=%llu\ndgrams_out_s=%.1f\nack_only_out=%llu\nack_only_s=%.1f\nack_data_out=%llu\ndata_only_out=%llu\ndgrams_in=%llu\ndgrams_in_s=%.1f\n",
           (unsigned long long)g_ns.dgrams_out, g_ns.dgrams_out / secs, (unsigned long long)g_ns.ack_only_out, g_ns.ack_only_out / secs,
           (unsigned long long)g_ns.ack_data_out, (unsigned long long)g_ns.data_only_out, (unsigned long long)g_ns.dgrams_in, g_ns.dgrams_in / secs);
    if (nwin) {
        qsort(win_gap, nwin, sizeof(uint64_t), cmp_u64); qsort(win_dg, nwin, sizeof(uint64_t), cmp_u64); qsort(win_backlog, nwin, sizeof(uint64_t), cmp_u64);
        printf("win_n=%u\nwin_pause_p50=%.1f\nwin_pause_p95=%.1f\nwin_pause_max=%.1f\nwin_dgrams_p50=%llu\nwin_dgrams_p95=%llu\nwin_dgrams_max=%llu\nwin_backlog_p50=%llu\nwin_backlog_p95=%llu\nwin_backlog_max=%llu\n", nwin,
               win_gap[nwin / 2] / 1000.0, win_gap[nwin * 95 / 100] / 1000.0, win_gap[nwin - 1] / 1000.0, (unsigned long long)win_dg[nwin / 2],
               (unsigned long long)win_dg[nwin * 95 / 100], (unsigned long long)win_dg[nwin - 1], (unsigned long long)win_backlog[nwin / 2],
               (unsigned long long)win_backlog[nwin * 95 / 100], (unsigned long long)win_backlog[nwin - 1]);
    }
    printf("svc_max_ms=%.3f\nround_max_ms=%.3f\nloop_gap_max_ms=%.3f\ndrained_max=%llu\ndrained_gt8=%llu\n", svc_max_us / 1000.0, round_max_us / 1000.0, loop_gap_max_us / 1000.0,
           (unsigned long long)drained_max, (unsigned long long)drained_hist_gt8);
    printf("room_rtt=%u\nroom_rtt_var=%u\nroom_rto=%u\nroom_rtt_var_max=%u\nroom_rto_max=%u\nroom_rx_packets=%u\nroom_disconnects=%u\n", rs.rtt, rs.rtt_var, rs.rto, rs.rtt_var_max,
           rs.rto_max, rs.rx_packets, rs.disconnects);
    printf("client_rtt=%u\nclient_rtt_var=%u\nrecoveries=%llu\npings_sent=%llu\nsilence_probes=%llu\n", server->roundTripTime, server->roundTripTimeVariance, (unsigned long long)recoveries, (unsigned long long)pings_sent, (unsigned long long)probes);
    {
        uint64_t retx = 0, spurious = 0, lost_orig = 0;
        for (uint32_t i = 0; i < g_frames_injected && i < VS_MAX_FRAMES; ++i) {
            if (g_sent_cnt[i] > 1) retx += g_sent_cnt[i] - 1;
            lost_orig += g_lost_cnt[i];
            const int extra = (int)g_sent_cnt[i] - 1 - (int)g_lost_cnt[i];
            if (extra > 0) spurious += (uint64_t)extra;
        }
        printf("room_retransmits=%llu\nroom_retransmits_spurious=%llu\nwire_lost_down=%llu\n", (unsigned long long)retx, (unsigned long long)spurious, (unsigned long long)lost_orig);
    }
    /* latencies */
    {
        uint64_t* add = (uint64_t*)malloc((g_frames_injected + 1) * sizeof(uint64_t));
        uint64_t* e2e = (uint64_t*)malloc((g_frames_injected + 1) * sizeof(uint64_t));
        uint64_t* wirel = (uint64_t*)malloc((g_frames_injected + 1) * sizeof(uint64_t));
        size_t na = 0, ne = 0, nw = 0, missing = 0;
        FILE* df = cfg.dump[0] ? fopen(cfg.dump, "w") : NULL;
        if (df) fprintf(df, "id,inject_us,arrive_us,deliver_us,sent_cnt\n");
        for (uint32_t i = 0; i < g_frames_injected; ++i) {
            if (t_inject[i] < t_base_us + 500000ull) continue;     /* (the first half second: warm-up) */
            if (t_deliver[i] && t_arrive[i]) { add[na++] = t_deliver[i] - t_arrive[i]; e2e[ne++] = t_deliver[i] - t_inject[i]; }
            else if (t_inject[i] + 2000000ull < vt_us) ++missing;
            if (t_arrive[i]) wirel[nw++] = t_arrive[i] - t_inject[i];
            if (df) fprintf(df, "%u,%llu,%llu,%llu,%u\n", i, (unsigned long long)t_inject[i], (unsigned long long)t_arrive[i], (unsigned long long)t_deliver[i], g_sent_cnt[i]);
        }
        if (df) fclose(df);
        pct("added_ms", add, na);
        pct("e2e_ms", e2e, ne);
        pct("wire_ms", wirel, nw);
        printf("frames_never_delivered=%zu\n", missing);
    }
    {
        qsort(tx_wait, n_txwait, sizeof(uint64_t), cmp_u64);
        if (n_txwait) printf("txwait_p50=%.3f\ntxwait_p95=%.3f\ntxwait_p99=%.3f\n", tx_wait[n_txwait * 50 / 100] / 1000.0, tx_wait[n_txwait * 95 / 100] / 1000.0, tx_wait[n_txwait * 99 / 100] / 1000.0);
    }
#ifdef VSIM_KNOBS
    {
        ENetAzlCounters c;
        enet_azl_counters_take(client, &c);
        printf("enet_passes=%u\nenet_pass_dgrams_max=%u\nenet_ooo=%u\nenet_dup=%u\nenet_ack_only=%u\nenet_ack_data=%u\nenet_recv_skipped=%u\n", c.passes, c.passDatagramsMax, c.oooCommands,
               c.dupCommands, c.ackOnlyDatagrams, c.ackDataDatagrams, c.recvSkipped);
        printf("enet_passes_capped=%u\nenet_events_before_ack=%u\nenet_ack_limit_hits=%u\nenet_ack_hold_max_ms=%u\nenet_hol_max_ms=%u\n", c.passesCapped, c.eventsBeforeAck, c.ackHoldLimitHits,
               c.ackHoldMaxMs, c.holWaitMaxMs);
    }
#endif
    printf("client_state=%d\npackets_lost_client=%u\n", (int)server->state, server->packetsLost);
    return 0;
}
