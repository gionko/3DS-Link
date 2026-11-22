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
    /* the loop's two reliefs for a dense broadcast stream (azl/flow_relief.hpp); the defaults are the loop every other game has */
    int burst;                /* packets of the room handed over in one round (1: one, then the queue and the flush) */
    double same_ms;           /* a frame of the game equal to the last one sent and younger than this is not sent (0: all are sent) */
    /* the closed-loop game (game=1): see the block "the game" below */
    int game;
    double game_from_s, game_secs, game_frame_us, game_step_us, game_timeout_s;
    int game_window, game_head_b, game_block_b, game_wrap_b;
    int game_players;         /* players of the game, the console included (2: the console and the other side) */
    int cap_drop;             /* (plugin-causa) 1: ENet acknowledges and drops a frame at the limit instead of refusing it (ENetHost::azlCapDrop; a tree that has it) */
    int cap_drop_max;         /* (plugin-causa) the bound of the placeholders of dropped frames (ENetHost::azlCapDropMax; 0 = ENet's default, 512) */
    int send_budget;          /* (2026-11-15) bytes of the game's queue handed to ENet in a round, the first frame always (RoomClient::SetSendBudget; 0 = all of it) */
    double ring_first_ms;     /* (2026-11-15) with a budget: after an absence the queue waits while the ring's backlog is read, this long at the most (flow::kAwayDrainMaxMs; 0 = off) */
} Cfg;
static Cfg cfg = {"run", "", 60, 1, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1, 0, 1, 0, 0, 2, 30, 5556, 33333, 10, 12, 32, 46, 41, 2, 0};

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
    else if (!strcmp(k, "burst")) cfg.burst = (int)d;
    else if (!strcmp(k, "same_ms")) cfg.same_ms = d;
    else if (!strcmp(k, "game")) cfg.game = (int)d;
    else if (!strcmp(k, "game_from_s")) cfg.game_from_s = d;
    else if (!strcmp(k, "game_secs")) cfg.game_secs = d;
    else if (!strcmp(k, "game_frame_us")) cfg.game_frame_us = d;
    else if (!strcmp(k, "game_step_us")) cfg.game_step_us = d;
    else if (!strcmp(k, "game_timeout_s")) cfg.game_timeout_s = d;
    else if (!strcmp(k, "game_window")) cfg.game_window = (int)d;
    else if (!strcmp(k, "game_head_b")) cfg.game_head_b = (int)d;
    else if (!strcmp(k, "game_block_b")) cfg.game_block_b = (int)d;
    else if (!strcmp(k, "game_wrap_b")) cfg.game_wrap_b = (int)d;
    else if (!strcmp(k, "game_players")) cfg.game_players = (int)d;
    else if (!strcmp(k, "cap_drop")) cfg.cap_drop = (int)d;
    else if (!strcmp(k, "cap_drop_max")) cfg.cap_drop_max = (int)d;
    else if (!strcmp(k, "send_budget")) cfg.send_budget = (int)d;
    else if (!strcmp(k, "ring_first_ms")) cfg.ring_first_ms = d;
    else if (!strcmp(k, "rcvbuf_bytes")) g_rcvbuf_bytes = (uint32_t)d;
    else if (!strcmp(k, "send_kb_us")) g_prof.send_kb_us = d;
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

#ifdef VSIM_CAPDROP
static int vsim_every_frame(const enet_uint8* data, size_t length) { (void)data; (void)length; return 1; }
#endif

/* ---------------------------------------------------------------- the trace */
typedef struct { uint64_t t_us; uint32_t size, id; } InjEv;
typedef struct { uint64_t t_us; uint32_t size; int rel; uint32_t id; } TxEv;
static InjEv* inj; static size_t n_inj, inj_next;
static TxEv* txe; static size_t n_tx, tx_next, tx_cap;
static uint64_t t_base_us = 3000000; /* the traffic starts 3 s into the run: the connection is up by then */

static void load_trace(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { perror(path); exit(2); }
    size_t cap_i = 1 << 16, cap_t = 1 << 16;
    inj = (InjEv*)malloc(cap_i * sizeof(InjEv));
    txe = (TxEv*)malloc(cap_t * sizeof(TxEv));
    tx_cap = cap_t;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char kind;
        double t; unsigned size; int rel = 0;
        if (line[0] == 'R' && sscanf(line, "%c %lf %u", &kind, &t, &size) == 3) {
            if (n_inj == cap_i) { cap_i *= 2; inj = (InjEv*)realloc(inj, cap_i * sizeof(InjEv)); }
            inj[n_inj].t_us = t_base_us + (uint64_t)t; inj[n_inj].size = size; inj[n_inj].id = (uint32_t)n_inj; ++n_inj;
        } else if (line[0] == 'T' && sscanf(line, "%c %lf %u %d", &kind, &t, &size, &rel) >= 3) {
            if (n_tx == cap_t) { cap_t *= 2; txe = (TxEv*)realloc(txe, cap_t * sizeof(TxEv)); tx_cap = cap_t; }
            txe[n_tx].t_us = t_base_us + (uint64_t)t; txe[n_tx].size = size; txe[n_tx].rel = rel; txe[n_tx].id = 0x80000000u | (uint32_t)n_tx; ++n_tx;
        }
    }
    fclose(f);
    if (n_inj >= VS_MAX_FRAMES) { fprintf(stderr, "vsim: trace too long\n"); exit(2); }
}

/* ---------------------------------------------------------------- the game (game=1): a closed loop
   The dense broadcast phase of Mario Party: Island Tour (the dice), as the captures of 2026-11-13/14 show it on data channel 1: each side makes
   2 new blocks every step (33 ms) and sends a frame every 5.5 ms, 180 a second, with ALL its blocks the other has not confirmed yet (2 at
   the least, 12 at the most: at 12 it makes no new ones and waits) and the number of the last block it has from the other. A frame is
   head + blocks * block bytes (124 B with 2 blocks, 584 B with 12) and travels in the room's envelope. So the bytes follow the delay: the later
   the confirmations, the larger every frame, 180 times a second in both directions. Here the console's game (its frames are queued for the loop
   as the T lines of a trace are) and the other player's (an Azahar beside the room: its frames are handed to the room as the R lines are, and it
   hears the console's when the room has them) run that rule on the model's clock. A frame's bytes are told by (blocks made, blocks confirmed,
   last block heard): equal numbers, equal frame - what the same-frame filter looks at. The console's game is told of a frame when the loop
   hands it over (it reads its bind at once); a side that hears nothing for game_timeout_s has lost the other (the game's own 10 s). */
typedef struct { uint32_t made, confirmed, heard; } GameMeta; /* of a frame: the blocks its sender had made and had confirmed, the last block of the other it had heard */
typedef struct {
    uint32_t made, confirmed, heard; /* blocks made; of them, confirmed by the other; the last block of the other heard */
    uint64_t next_step_us, next_frame_us, last_heard_us;
    uint64_t steps, frames, bytes, dead_at_us, gap_max_us;
} GameSide;
static GameSide g_con, g_azh;
static GameMeta* g_meta_r; /* by the id of a frame of the room (0 ..) */
static GameMeta* g_meta_t; /* by the index of a frame of the console */
static size_t g_meta_t_cap;
static uint64_t g_game_from_us, g_game_to_us, g_same_dropped;
static uint32_t g_room_ids;
static GameMeta g_last_sent;
static uint64_t g_last_sent_us;
static int g_last_sent_any;
static uint64_t g_room_now_us; /* the room's clock while its events run */
static void game_steps(GameSide* s, uint64_t t) {
    for (; s->next_step_us <= t; s->next_step_us += (uint64_t)cfg.game_step_us) {
        if (s->made - s->confirmed + 2 <= (uint32_t)cfg.game_window) { s->made += 2; ++s->steps; }
    }
}
static uint32_t game_frame_size(const GameSide* s) {
    uint32_t blocks = s->made - s->confirmed;
    if (blocks < 2) blocks = 2;
    if (blocks > (uint32_t)cfg.game_window) blocks = (uint32_t)cfg.game_window;
    return (uint32_t)(cfg.game_head_b + cfg.game_block_b * (int)blocks + cfg.game_wrap_b);
}
static void game_hears(GameSide* s, GameMeta m, uint64_t t) {
    if (s->dead_at_us) return;
    if (s->last_heard_us && t - s->last_heard_us > s->gap_max_us) s->gap_max_us = t - s->last_heard_us;
    s->last_heard_us = t;
    if (m.heard > s->confirmed) s->confirmed = m.heard;
    if (m.made > s->heard) s->heard = m.made;
}
static void game_silence(GameSide* s, uint64_t t) {
    if (!s->dead_at_us && s->last_heard_us && t > s->last_heard_us && t - s->last_heard_us >= (uint64_t)(cfg.game_timeout_s * 1e6))
        s->dead_at_us = s->last_heard_us + (uint64_t)(cfg.game_timeout_s * 1e6);
}
void vsim_on_room_rx(uint32_t id) {
    if (!cfg.game || !(id & 0x80000000u)) return;
    const size_t i = id & 0x7FFFFFFFu;
    if (i < g_meta_t_cap) game_hears(&g_azh, g_meta_t[i], g_room_now_us);
}

/* ---------------------------------------------------------------- the room's events in time order */
static uint64_t room_last_run = 0;
uint64_t vsim_next_room_event_us(void) {
    uint64_t t = room_last_run + 16000ull; /* ServerLoop: enet_host_service(server, &event, 16) */
    if (cfg.game && g_azh.next_frame_us < g_game_to_us && !g_azh.dead_at_us && g_azh.next_frame_us < t) t = g_azh.next_frame_us;
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
        g_room_now_us = t;
        /* the other player's frames of this instant: handed to the room as the trace's are */
        while (cfg.game && g_azh.next_frame_us <= t && g_azh.next_frame_us < g_game_to_us && !g_azh.dead_at_us) {
            game_silence(&g_azh, g_azh.next_frame_us);
            if (g_azh.dead_at_us) break;
            game_steps(&g_azh, g_azh.next_frame_us);
            if (g_room_ids < VS_MAX_FRAMES) {
                const uint32_t id = g_room_ids++;
                const uint32_t size = game_frame_size(&g_azh);
                g_meta_r[id].made = g_azh.made;
                g_meta_r[id].confirmed = g_azh.confirmed;
                g_meta_r[id].heard = g_azh.heard;
                t_inject[id] = g_azh.next_frame_us;
                room_inject(id, size);
                ++g_frames_injected;
                ++g_azh.frames;
                g_azh.bytes += size;
                /* a game of more than two (game_players): every other player sends what the modelled one sends, at its pace and size - a
                   stand-in, never seen on a console: the room's stream to the console grows by 180 frames a second for each */
                for (int extra = 2; extra < cfg.game_players && g_room_ids < VS_MAX_FRAMES; ++extra) {
                    const uint32_t id2 = g_room_ids++;
                    g_meta_r[id2] = g_meta_r[id];
                    t_inject[id2] = g_azh.next_frame_us;
                    room_inject(id2, size);
                    ++g_frames_injected;
                }
            }
            g_azh.next_frame_us += (uint64_t)cfg.game_frame_us;
        }
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

/* The console's game up to `t`: the frames it sends are queued for the loop (appended to the trace's T events, in time order). With same_ms the frame
   that is equal to the last one SENT and younger than same_ms is not queued (azl/flow_relief.hpp, SameFrame: the engine drops it in SendTo). */
static void game_console_until(uint64_t t) {
    if (!cfg.game) return;
    while (g_con.next_frame_us <= t && g_con.next_frame_us < g_game_to_us && !g_con.dead_at_us) {
        const uint64_t te = g_con.next_frame_us;
        game_silence(&g_con, te);
        if (g_con.dead_at_us) break;
        game_steps(&g_con, te);
        g_con.next_frame_us += (uint64_t)cfg.game_frame_us;
        ++g_con.frames;
        const GameMeta m = {g_con.made, g_con.confirmed, g_con.heard};
        if (cfg.same_ms > 0 && g_last_sent_any && m.made == g_last_sent.made && m.confirmed == g_last_sent.confirmed && m.heard == g_last_sent.heard &&
            te - g_last_sent_us < (uint64_t)(cfg.same_ms * 1000.0)) {
            ++g_same_dropped;
            continue;
        }
        g_last_sent = m;
        g_last_sent_us = te;
        g_last_sent_any = 1;
        if (n_tx == tx_cap) {
            tx_cap = tx_cap ? tx_cap * 2 : 1 << 16;
            txe = (TxEv*)realloc(txe, tx_cap * sizeof(TxEv));
        }
        if (n_tx >= g_meta_t_cap) {
            g_meta_t_cap = g_meta_t_cap ? g_meta_t_cap * 2 : 1 << 16;
            g_meta_t = (GameMeta*)realloc(g_meta_t, g_meta_t_cap * sizeof(GameMeta));
        }
        g_meta_t[n_tx] = m;
        txe[n_tx].t_us = te;
        txe[n_tx].size = game_frame_size(&g_con);
        txe[n_tx].rel = 0;
        txe[n_tx].id = 0x80000000u | (uint32_t)n_tx;
        g_con.bytes += txe[n_tx].size;
        ++n_tx;
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
    if (argc < 3) { fprintf(stderr, "usage: vsim <config> <trace> [key=value ...]\n"); return 2; }
    for (int k = 0; k < SOC_KINDS_N; ++k) g_prof.s20[k] = g_prof.s100[k] = -1.0;
    load_cfg(argv[1]);
    for (int i = 3; i < argc; ++i) { /* (keys given after the two files replace the configuration's: a variant of a run without a second file) */
        char pair[256];
        snprintf(pair, sizeof pair, "%s", argv[i]);
        char* eq = strchr(pair, '=');
        if (!eq) { fprintf(stderr, "vsim: %s is not key=value\n", argv[i]); return 2; }
        *eq = 0;
        cfg_set(pair, eq + 1);
    }
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
#ifdef VSIM_CAPDROP
    if (cfg.cap_drop) client->azlCapDrop = vsim_every_frame; /* (in the model every packet of the room is a game frame) */
    if (cfg.cap_drop_max > 0) client->azlCapDropMax = (enet_uint32)cfg.cap_drop_max;
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
    /* (the frames of the trace, and with game=1 the ones the console's game will make) */
    uint64_t* tx_wait = (uint64_t*)malloc((n_tx + 1 + (cfg.game ? (size_t)(cfg.game_secs * 1e6 / cfg.game_frame_us) + 16 : 0)) * sizeof(uint64_t));
    size_t n_txwait = 0;
    uint64_t drained_max = 0, drained_hist_gt8 = 0;
    uint64_t burst_extra = 0, burst_max = 0;
    size_t tx_queue_max = 0;
    uint64_t budget_rounds = 0, drain_rounds = 0, away_since = 0; /* (2026-11-15: the send budget and the ring read first) */
    (void)away_since;
    if (cfg.game) {
        if (n_inj || n_tx) { fprintf(stderr, "vsim: game=1 wants an empty trace\n"); return 2; }
        g_meta_r = (GameMeta*)calloc(VS_MAX_FRAMES, sizeof(GameMeta));
        g_game_from_us = t_base_us + (uint64_t)(cfg.game_from_s * 1e6);
        g_game_to_us = g_game_from_us + (uint64_t)(cfg.game_secs * 1e6);
        g_con.next_step_us = g_con.next_frame_us = g_game_from_us;
        g_azh.next_step_us = g_azh.next_frame_us = g_game_from_us + (uint64_t)(cfg.game_frame_us / 2); /* (the two consoles' clocks are not in step) */
        g_con.last_heard_us = g_azh.last_heard_us = g_game_from_us;
    }
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
    /* (plugin-causa) the room's side of the console's hole, sampled every 100 ms; with VSIM_TL a line every 500 ms */
    uint64_t tl_next = t_base_us, tl_delivered = 0, delivered = 0, last_deliver_us = 0, deliver_gap_max_us = 0, deliver_gap_at_us = 0;
    uint32_t hole_attempts_max = 0, room_attempts_max = 0, room_rto_max_cmd = 0, hole_next_max = 0;
    const int tl_print = getenv("VSIM_TL") != NULL;
    while (vt_us < t_end && !done) {
        if (vt_us >= tl_next) {
            RoomProbe rp;
            const uint16_t hole = (uint16_t)(server->channels[0].incomingReliableSequenceNumber + 1);
            room_probe(hole, &rp);
            if (rp.hole_attempts > hole_attempts_max) hole_attempts_max = rp.hole_attempts;
            if (rp.max_attempts > room_attempts_max) room_attempts_max = rp.max_attempts;
            if (rp.max_rto_ms > room_rto_max_cmd) room_rto_max_cmd = rp.max_rto_ms;
            if (rp.hole_where == 1 && rp.hole_next_ms > hole_next_max) hole_next_max = rp.hole_next_ms;
            if (tl_print && (tl_next - t_base_us) % 500000ull == 0) {
#ifdef VSIM_CAP
                const unsigned refused = client->azlCounters.capRefused, admitted = client->azlCounters.capAdmitted;
#else
                const unsigned refused = 0, admitted = 0;
#endif
                printf("tl t=%.1f waiting=%zu ready=%d delivered=%llu refused=%u admitted=%u | room unacked=%u resend_waiting=%u in_transit=%u thr=%u max_att=%u max_rto=%u"
                       " | awaited seq=%u where=%d att=%u rto=%u next_in=%u%s\n",
                       (tl_next - t_base_us) / 1e6, server->totalWaitingData, !enet_list_empty(&server->dispatchedCommands), (unsigned long long)(delivered - tl_delivered),
                       refused, admitted, rp.unacked, rp.resend_waiting, rp.in_transit, rp.throttle, rp.max_attempts, rp.max_rto_ms, hole, rp.hole_where, rp.hole_attempts,
                       rp.hole_rto_ms, rp.hole_next_ms, room_connected() ? "" : " | ROOM HAS DROPPED THE CONSOLE");
                tl_delivered = delivered;
            }
            tl_next += 100000ull;
        }
        const uint64_t round_start = vt_us;
        const uint64_t gap = round_start - last_round_start;
        if (gap > loop_gap_max_us) loop_gap_max_us = gap;
        last_round_start = round_start;
        ++rounds;
        if (gap > wgap) wgap = gap;
        if (gap > 40000 && getenv("VSIM_TRACE")) fprintf(stderr, "gap %.1f ms before t=%.3f s (backlog %u)\n", gap / 1000.0, (vt_us - t_base_us) / 1e6, vnet_client_backlog(vt_us));
        { const uint32_t bl = vnet_client_backlog(vt_us); if (bl > wbl) wbl = bl; }
        if (vt_us - win_start >= 5000000ull && nwin < MAXWIN) { win_gap[nwin] = wgap; win_dg[nwin] = wdg; win_backlog[nwin] = wbl; ++nwin; wgap = wdg = wbl = 0; win_start = vt_us; }
        game_console_until(vt_us);
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
            /* the packet the call handed over, and with `burst` the ones that are ready behind it: enet_host_check_events makes no soc call */
            int handed = 0;
            for (int have = r; have > 0 && !done; have = (++handed < cfg.burst) ? enet_host_check_events(client, &ev) : 0) {
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                    uint32_t id = 0xFFFFFFFFu;
                    if (ev.packet->dataLength >= 4) memcpy(&id, ev.packet->data, 4);
                    const double h = rnd_logn(g_prof.handler_us, 0.5);
                    handler_total_us += h;
                    vt_us += (uint64_t)h;
                    if (id < VS_MAX_FRAMES) t_deliver[id] = vt_us;
                    ++delivered;
                    if (last_deliver_us && vt_us - last_deliver_us > deliver_gap_max_us) { deliver_gap_max_us = vt_us - last_deliver_us; deliver_gap_at_us = vt_us; }
                    last_deliver_us = vt_us;
                    if (cfg.game && id < g_room_ids) {
                        game_console_until(vt_us); /* (what the game sent before it had this frame) */
                        game_hears(&g_con, g_meta_r[id], vt_us);
                    }
                    enet_packet_destroy(ev.packet);
                    if (handed > 0) ++burst_extra;
                } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                    lost_at = vt_us;
                    done = 1;
                }
            }
            if ((uint64_t)handed > burst_max) burst_max = (uint64_t)handed;
        }
        if (done) break;
        /* the game's frames into ENet, then a flush */
        int sent_any = 0;
        game_console_until(vt_us);
        if (tx_next < n_tx && n_tx - tx_next > tx_queue_max) {
            size_t waiting = 0;
            for (size_t i = tx_next; i < n_tx && txe[i].t_us <= vt_us; ++i) ++waiting;
            if (waiting > tx_queue_max) tx_queue_max = waiting;
        }
        /* (2026-11-15) the titles with a send budget: after an absence, the ring's backlog first (RoomClient::Run, flow::HoldsForTheRing) */
        int hold = 0;
#ifdef VSIM_AWAY
        if (client->azlAwayBacklog) {
            if (!away_since) away_since = vt_us ? vt_us : 1;
        } else {
            away_since = 0;
        }
        if (cfg.send_budget > 0 && cfg.ring_first_ms > 0 && away_since && vt_us - away_since < (uint64_t)(cfg.ring_first_ms * 1000.0) &&
            tx_next < n_tx && txe[tx_next].t_us <= vt_us) {
            hold = 1;
            ++drain_rounds;
        }
#endif
        uint32_t budget_used = 0;
        int took = 0;
        while (!hold && tx_next < n_tx && txe[tx_next].t_us <= vt_us) {
            const TxEv* t = &txe[tx_next];
            const uint32_t size = t->size < 8 ? 8 : (t->size > 1500 ? 1500 : t->size);
            if (cfg.send_budget > 0 && took && budget_used + size > (uint32_t)cfg.send_budget) {
                ++budget_rounds; /* (TxQueue::TakeUpTo: the rest waits for the next round, which reads first) */
                break;
            }
            budget_used += size;
            took = 1;
            ++tx_next;
            uint8_t buf[1600];
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
#ifdef VSIM_CAP
        printf("enet_cap_refused=%u\nenet_cap_admitted=%u\nenet_waiting_max=%u\nenet_stream_lag_max_ms=%u\n", c.capRefused, c.capAdmitted, c.waitingMaxBytes, c.streamLagMaxMs);
#endif
#ifdef VSIM_CAPDROP
        printf("enet_cap_dropped=%u\nenet_cap_drop_held_now=%u\n", c.capDropped, client->azlCapDropHeld);
#ifdef ENET_AZL_AWAY_MS
        printf("enet_cap_full=%u\nenet_cap_held_max=%u\nenet_seq_skipped=%u\nenet_seq_skipped_away=%u\n", c.capDropFull, c.capDropHeldMax, c.seqSkipped, c.seqSkippedAway);
#endif
#endif
    }
#endif
    printf("client_state=%d\npackets_lost_client=%u\n", (int)server->state, server->packetsLost);
    /* (plugin-causa) the room's side of the holes; the longest the console's game went without a frame of the room (from the end too, if it never came back) */
    if (last_deliver_us && t_end > last_deliver_us && (vt_us - last_deliver_us) > deliver_gap_max_us && inj_next > 0) { deliver_gap_max_us = vt_us - last_deliver_us; deliver_gap_at_us = vt_us; }
    printf("room_hole_attempts_max=%u\nroom_cmd_attempts_max=%u\nroom_cmd_rto_max=%u\nroom_hole_next_max=%u\ndeliver_gap_max_ms=%.1f\ndeliver_gap_at_s=%.3f\ndelivered=%llu\n",
           hole_attempts_max, room_attempts_max, room_rto_max_cmd, hole_next_max, deliver_gap_max_us / 1000.0,
           deliver_gap_at_us ? (deliver_gap_at_us - t_base_us) / 1e6 : -1.0, (unsigned long long)delivered);
    if (cfg.send_budget > 0) printf("send_budget=%d\nring_first_ms=%.1f\nbudget_rounds=%llu\ndrain_rounds=%llu\n", cfg.send_budget, cfg.ring_first_ms,
                                    (unsigned long long)budget_rounds, (unsigned long long)drain_rounds);
#ifdef VSIM_AWAY
    {
        ENetAzlCounters c2 = client->azlCounters; /* (the away counters of N1; the window above took the others) */
        printf("enet_away_episodes=%u\nenet_away_dgrams_max=%u\nenet_away_bytes_max=%u\nenet_away_dup_max=%u\n", c2.awayEpisodes, c2.awayDatagramsMax, c2.awayBytesMax, c2.awayDupMax);
    }
#endif
    if (cfg.burst > 1 || cfg.same_ms > 0 || cfg.game || g_rcvbuf_bytes) {
        printf("burst=%d\nburst_extra=%llu\nburst_max=%llu\nsame_ms=%.1f\nsame_dropped=%llu\ntx_queue_max=%zu\nring_drops=%llu\nring_peak_bytes=%u\nwaiting_now=%zu\n", cfg.burst,
               (unsigned long long)burst_extra, (unsigned long long)burst_max, cfg.same_ms, (unsigned long long)g_same_dropped, tx_queue_max, (unsigned long long)g_ns.ring_drops,
               g_ns.ring_peak_bytes, server->totalWaitingData);
    }
    if (cfg.game) {
        const double gs = cfg.game_secs;
        game_silence(&g_con, vt_us < g_game_to_us ? vt_us : g_game_to_us);
        game_silence(&g_azh, vt_us < g_game_to_us ? vt_us : g_game_to_us);
        printf("game_secs=%.1f\ngame_con_steps_s=%.2f\ngame_azh_steps_s=%.2f\ngame_con_frames=%llu\ngame_azh_frames=%llu\ngame_con_frame_b=%.0f\ngame_azh_frame_b=%.0f\n", gs, g_con.steps / gs,
               g_azh.steps / gs, (unsigned long long)g_con.frames, (unsigned long long)g_azh.frames, g_con.frames ? (double)g_con.bytes / (double)(g_con.frames - g_same_dropped) : 0.0,
               g_azh.frames ? (double)g_azh.bytes / (double)g_azh.frames : 0.0);
        printf("game_con_gap_max_ms=%.1f\ngame_azh_gap_max_ms=%.1f\ngame_con_dead_at_s=%.3f\ngame_azh_dead_at_s=%.3f\n", g_con.gap_max_us / 1000.0, g_azh.gap_max_us / 1000.0,
               g_con.dead_at_us ? (g_con.dead_at_us - g_game_from_us) / 1e6 : -1.0, g_azh.dead_at_us ? (g_azh.dead_at_us - g_game_from_us) / 1e6 : -1.0);
    }
    return 0;
}
