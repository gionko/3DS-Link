/* vsim: a virtual-time model of the plugin's network thread (RoomClient::Run) talking to a room over a wire with a delay.
 *
 * What is REAL: the ENet code of the plugin (Sources/enet, whichever tree the model is built against, with the plugin's own platform
 * layer Sources/enet/enet_3ds.c), and, on the room's side, ENet 1.3.18 as Azahar's room runs it (lsalzman/enet v1.3.18, unmodified,
 * its symbols renamed r_enet_* so that both can live in one program).
 * What is MODELLED: the clock (microseconds, virtual), the wire (delay, jitter, loss, an outage), the sockets of the soc service (what a
 * call costs on the console, from the plugin's own statistics windows), the room's application (it hands every frame to the console as
 * a reliable packet and flushes, room.cpp), the frames of the game (a trace) and the loop of the network thread (room_client.cpp).
 * Nothing sleeps: a run of ten minutes takes a second. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the clock: every actor runs at vt_us while it runs ---- */
extern uint64_t vt_us;
extern int vt_actor; /* 0: the console's network thread, 1: the room */
uint64_t vsim_now_us(void);

/* ---- random numbers (xorshift, seeded) ---- */
void rnd_seed(uint64_t s);
double rnd_u(void);                    /* uniform in [0,1) */
double rnd_norm(void);                 /* standard normal */
double rnd_logn(double mean, double cv); /* lognormal with that mean and coefficient of variation */

/* ---- what the soc calls of the console cost ---- */
#define SOC_KINDS_N 5
typedef struct {
    double recv_us, recvE_us, send_us, pollr_us, wake_us; /* means: a receive with data, an empty one, a send, a poll that returns at once, the wake-up of a poll that waited */
    double cv;                                           /* spread of a call (lognormal) */
    double slow20_p, slow100_p;                          /* (all kinds, the default) per call: chance of +20..100 ms (the thread was not scheduled) and of +100..max ms */
    double s20[SOC_KINDS_N], s100[SOC_KINDS_N];          /* the same by kind of call: recv, recvE, send, pollr, wake; < 0 = the default above */
    double slow_max_ms, slow_scale_ms;
    double handler_us;                                   /* a frame handed to the engine (decode, lock, ring): per packet */
    double round_us;                                     /* the tail of a round of the loop (queue, tick) */
} Profile;
extern Profile g_prof;

/* ---- the wire ---- */
typedef struct {
    double up_base_us, up_sd_us;     /* console -> room */
    double down_base_us, down_sd_us; /* room -> console */
    double loss;                     /* chance a datagram is lost (each direction) */
} NetCfg;
extern NetCfg g_net;
extern int g_outage_silent;                       /* 1: the datagrams vanish and the calls succeed (a WiFi that went away unnoticed); 0: the calls fail with ENETDOWN */
extern uint64_t g_outage_from_us, g_outage_to_us; /* the console's socket fails in [from, to): sendto -> ENETDOWN, the wire drops both ways */
extern uint32_t g_drop_connects;                  /* the room does not hear the next N CONNECT datagrams of the console */

enum { SOC_RECV = 0, SOC_RECVE, SOC_SEND, SOC_POLLR, SOC_POLLW, SOC_KINDS };
typedef struct {
    uint64_t n[SOC_KINDS];
    double us[SOC_KINDS];
    uint64_t dgrams_in, dgrams_out, ack_only_out, ack_data_out, data_only_out;
    uint64_t ring_drops;
    uint32_t ring_peak_bytes;
} NetStats;
extern NetStats g_ns;

/* the datagram queues */
uint64_t vnet_client_next_arrival(void);
uint64_t vnet_room_next_arrival(void);
uint32_t vnet_client_backlog(uint64_t now_us);   /* datagrams that have arrived and are not read yet (the soc ring) */
int vnet_room_recv(uint8_t* buf, size_t cap, uint64_t now_us);          /* >0 length, 0 none arrived yet */
int vnet_room_send(const uint8_t* buf, size_t len, uint64_t now_us);    /* the room puts a datagram on the wire to the console */

/* the frames of the room (ids), as the console sees them */
#define VS_MAX_FRAMES 400000
extern uint64_t t_inject[VS_MAX_FRAMES], t_arrive[VS_MAX_FRAMES], t_deliver[VS_MAX_FRAMES];
extern uint32_t g_frames_injected;
extern uint8_t g_sent_cnt[VS_MAX_FRAMES], g_lost_cnt[VS_MAX_FRAMES]; /* times the room sent / the wire lost a frame (a second send is a retransmission) */

/* ---- the room (room_side.c, ENet 1.3.18) ---- */
typedef struct {
    uint32_t connected;
    uint32_t packets_lost;      /* reliable commands that timed out at the room (retransmissions) */
    uint32_t rtt, rtt_var;      /* the room's estimate for the console, ms */
    uint32_t rto;               /* rtt + 4 var */
    uint32_t rtt_var_max, rto_max;
    uint32_t disconnects;
    uint32_t rx_packets;        /* packets of the console it received */
    uint64_t total_sent_dgrams, total_recv_dgrams;
} RoomStats;
void room_init(void);
int room_connected(void);
void room_inject(uint32_t id, uint32_t size);
void room_service(void);
void room_get_stats(RoomStats* out);
void room_set_now(uint64_t now_us);   /* the room's clock (its platform layer reads it) */

/* ---- the driver (vsim_client.c): the room's events in time order ---- */
void vsim_run_room_until(uint64_t t_us);
uint64_t vsim_next_room_event_us(void);

#ifdef __cplusplus
}
#endif
