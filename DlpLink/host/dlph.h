// dlph: Download Play HOST for 3DS Link, written on raw nwm::UDS (libctru). The stock Download Play of another console lists
// "3DS Link", downloads the DLP child CIA from here, installs it and launches it by itself.
//
// One Download Play console is served at a time. Two ways of keeping the room usable (hardware tests 2026-10-14..20,
// see DlpLink/docs/research and DlpLink/test/RISULTATI-*.md): a seat used by a Download Play console is "dirty" - the next
// console that lands on it stays half-connected for 44-49 s - and a network never recovers from a silent download.
//   * persistent = false, ROOM OF 2 (this console + ONE seat): the room is RE-MADE (udsDestroyNetwork + udsCreateNetwork)
//     every time the client has left and whenever it goes silent for 9 s; a new network has clean seats. Takes 1-4 s.
//     THIS IS THE MODE OF 3DS LINK 1.0 ("3DS Link Delivery" in the main menu), confirmed on hardware 2026-10-20: 11 consoles one after
//     the other, 0 lost, the room re-made in 1.1 s each time, the name of the console read within 70 ms. Use gap_us = 3000 (see below).
//   * persistent = true, a room kept up together with other nodes (not in 1.0; written and tested on PC only): the room is
//     NEVER re-made for a console that did its job. While the lowest free seat is dirty the library closes the door
//     (udsSetNewConnectionsBlocked) and stops announcing the title; after quarantine_ms the seat is clean and the door
//     reopens. Nodes that do not answer as Download Play consoles are left alone. A client that goes silent for 9 s
//     still makes the room be re-made (the only cure known).
//   * Selective resend (both): the client says which fragments of a block are missing; only those are sent again.
//
// THREADS: dlph_open / dlph_start / dlph_poll / dlph_stop / dlph_close from ONE thread (the "radio thread": dlph_poll blocks
// for up to ~0.6 s while it sends a block and 1-4 s while it re-makes the room, so keep it out of the UI thread).
// dlph_get_stats, dlph_history, dlph_request_kick and dlph_set_gap_us can be called from ANY thread (the UI thread):
// the stats are a snapshot published by the radio thread after every poll and every 16 fragments.
// The caller owns udsInit/udsExit and must not hold another UDS network while the room is open (one network per console).
#ifndef DLPH_H
#define DLPH_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DLPH_ROOM_DOWN = 0,         // no network
    DLPH_ROOM_OPEN,             // room up, beacon on, waiting for a console
    DLPH_CLIENT_JOINING,        // a console is in: handshake
    DLPH_CLIENT_DOWNLOADING,    // the child is being sent
    DLPH_CLIENT_FINISHING       // the child is delivered, the start packet is being sent
} dlph_state;

typedef enum {
    DLPH_EV_ROOM_OPEN,          // the room exists (arg 0)
    DLPH_EV_CLIENT_JOINED,      // a console entered the room and answered as a Download Play console (arg = seat)
    DLPH_EV_PROGRESS,           // arg = percent 0..100 of the download
    DLPH_EV_CLIENT_SERVED,      // the console has the app and left to install it
    DLPH_EV_CLIENT_LOST,        // the console left, went silent or was sent away before the end
    DLPH_EV_ROOM_REMADE,        // the room has been re-made (arg = 1 when because of a stalled client)
    DLPH_EV_CLIENT_NAME         // the user name of the console is known now (read it in dlph_get_stats().client_name)
} dlph_event;

typedef struct {
    const char *child_path;     // CIA of the DLP child, e.g. "sdmc:/3ds/3DSLink/dlp/child.cia" (see DlpLink/tools/make_dlp_child.sh)
    uint8_t mac[6];             // MAC address of THIS console: read it once with SOCU_GetNetworkOpt(NETOPT_MAC_ADDRESS)
    uint8_t iv[16];             // the 16-byte IV of the checksum of the fragments (the library does not carry it: the caller gives it; 3DS Link takes it from
                                // builtin_keys.h, the value Azahar ships as dlpChecksumModIv). All zeros = not given: dlph_open refuses
    uint8_t lang;               // SMDH language index for the name and description shown by the client; 0xFF = system language
    uint8_t max_nodes;          // 2..8; 0 = 2. Only for the room the library makes itself (dlph_start)
    bool persistent;            // false: ROOM OF 2, re-made after every console. true: the room stays up and a seat used by a console is
                                // kept closed for `quarantine_ms` (see below)
    uint32_t quarantine_ms;     // persistent mode: a seat whose Download Play console left is "dirty" for this long (default 45000): a console
                                // that lands on it stays half-connected until nwm forgets the old one (measured 44-49 s). While the LOWEST FREE
                                // seat is dirty, new connections are blocked and the title is hidden.
    int gap_us;                 // pause between two fragments. USE 3000: measured on hardware, one console, same child (2.65 MB):
                                //   3000 -> about 10 s, 1-3 fragments in 182 sent again (DlpTest/DlpTestRawHost-test22/rawhost_log.txt);
                                //   0    -> 17 s, 2.4 x the bytes on the air (6.2 MB for 2.6 MB): the radio takes ~2.9 ms per fragment anyway
                                //           (0.46-0.51 MiB/s on the air) and the client loses most of what arrives faster than it can read.
                                // So "full bandwidth" is not faster: the radio is the limit. Can be changed at any time with dlph_set_gap_us().
    void (*log)(const char *line, void *user);                // optional, developer lines (called from the radio thread)
    void (*event)(dlph_event ev, int arg, void *user);        // optional (called from the radio thread)
    void *user;
} dlph_cfg;

typedef struct {
    // the room
    dlph_state state;
    bool door_closed;           // persistent mode: new consoles are kept out while a used seat heals
    bool remaking;              // the room is being re-made (1-4 s): the screen can say so
    uint32_t served;            // consoles served since dlph_open
    uint32_t lost;              // consoles that left, went silent or were sent away before the end
    uint32_t remade;            // times the room has been re-made
    uint32_t room_ms;           // age of the current room (reset when it is re-made)
    uint32_t bytes_sent;        // child bytes put on the air since dlph_open (includes resends)
    // what is offered
    uint16_t child_version;     // title version announced (from the TMD of the child)
    uint64_t child_tid;
    uint32_t child_bytes;       // size of the child CIA (what the client downloads: transfer_bytes)
    uint32_t transfer_bytes;
    char child_name[64];        // UTF-8 short name from the SMDH, in the language of the announcement
    // the console that is being served (all zero / empty when none)
    int seat;                   // 2..8, 0 = nobody
    char client_name[32];       // UTF-8 user name of the console, "" until known (event DLPH_EV_CLIENT_NAME)
    int percent;                // 0..100 of the download
    uint32_t client_ms;         // since it answered as a Download Play console
    uint32_t silent_ms;         // since its last packet
    float mibps;                // child bytes on the air in the last second (resends included)
    int eta_s;                  // seconds left, -1 while unknown
    uint32_t frags_total;       // fragments of the child
    uint32_t frags_sent;        // fragments sent to this console (resends included)
    uint32_t frags_repeated;    // ... of which sent again
    uint32_t frags_refused;     // ... of which the radio refused (queue full): they count as sent, the console asks for them again
    bool already_has;           // the console said it already has this version of the app: nothing is sent (uninstall the child there, or raise the version)
} dlph_stats;

typedef enum { DLPH_V_SERVED = 0, DLPH_V_LOST, DLPH_V_KICKED } dlph_result;

typedef struct {                // one console that has been in the room
    char name[32];              // "" when it never gave one
    dlph_result result;
    int percent;                // how far it got
    bool already_had;           // it already had this version: "served" but nothing was transferred
    uint32_t duration_ms;
    uint32_t number;            // 1, 2, 3...: order of arrival since dlph_open
} dlph_visit;

bool dlph_open(const dlph_cfg *cfg);     // loads and checks the child, computes the checksum seed. Does not touch the network.
bool dlph_start(void);                   // creates the room
void dlph_poll(void);                    // call at >= 10 Hz from the radio thread. While a block is being sent it does not return for up to
                                         // ~0.6 s (182 fragments); when the room is re-made it blocks for 1-4 s.
void dlph_stop(void);                    // destroys the room ("Close room": stop polling, then call this)
void dlph_close(void);                   // frees the child
void dlph_set_gap_us(int gap_us);        // any thread
void dlph_request_kick(void);            // any thread: send away the console being served (applied by the next dlph_poll; the room is re-made)
dlph_stats dlph_get_stats(void);         // any thread
int dlph_history(dlph_visit *out, int max);       // any thread: the last consoles, NEWEST FIRST, at most 8; returns how many
const char *dlph_state_text(dlph_stats s);        // a one-line English text for the state, e.g. "Room open: waiting for a console"

#ifdef __cplusplus
}
#endif

#endif
