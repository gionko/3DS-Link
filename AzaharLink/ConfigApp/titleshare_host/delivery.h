// What 3DS Link's Titles Delivery screens ask of the delivery module (../titleshare), in plain C types.
//
// The module's own screens (its main.c, text on a console) are not used any more: the app draws everything itself. What the module does
// - the list of the installed titles, the check of a title, the room of the sender, the scan for senders, the receiving and the installing - is
// the module's code, called as it is (sender.c, receiver.c, catalog.c, ctx.c, power.c). This file is the place that includes the module's
// headers for the delivery screens (the scan of the titles, titles.cpp and ncch_icon.cpp, include the few they need by themselves): the headers
// have no C++ guards and some of their names (sched.h) hide a system header, so the screens only include THIS header, which is plain C and has
// no module type in it.
//
// The module asks its user's screen to draw through a callback it calls from inside its loops (4 times a second while sending; about 6.5 while
// receiving, and as often as it can while the receiver chooses): the app draws a frame in it and answers with what the user did.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DL_KIND_APP = 0, DL_KIND_UPDATE = 1, DL_KIND_DLC = 2, DL_KIND_OTHER = 3 };
// What a tick answers.
enum { DL_NONE = 0, DL_CONFIRM = 1, DL_CANCEL = 2 };

#define DL_MAX_TITLES 16  // the module's TS_MAX_TITLES: what one room can offer
#define DL_MAX_CLIENTS 8  // the module's TS_MAX_CLIENTS: receivers one sender serves
#define DL_NAME_BYTES 48
#define DL_CODE_BYTES 16

typedef struct {
    uint64_t tid;
    uint64_t size;
    uint8_t kind;
    uint8_t has_seed;
    char code[DL_CODE_BYTES];
    char name[DL_NAME_BYTES]; // the name of the game the title belongs to
} dl_title;

// ---------------------------------------------------------------------------------------------
// The module's state: the titles the app's own scan has already read (nothing is read again)
// ---------------------------------------------------------------------------------------------

// Hands over the module's context and catalog (ts_ctx and ts_catalog), as the app's scan made them. NULL, NULL detaches.
void dl_attach(void* ctx, void* catalog);
// Keys found (boot9.bin and the console's movable.sed): this console can send. Without them it can only receive.
int dl_have_keys(void);
int dl_is_new_3ds(void);

// The titles, grouped by game (a game, its update and its DLC under one name), games by name.
int dl_game_count(void);
const char* dl_game_name(int game);
int dl_game_member_count(int game);
int dl_game_member(int game, int k); // the index of the title (an item) in the list of all the titles
int dl_item_count(void);
uint64_t dl_item_tid(int item);
int dl_item_kind(int item);
const char* dl_item_code(int item);
const char* dl_item_name(int item); // the name of its game
// How big a title is, as the system knows it from what is installed (0 = not known): good for a list and for the time it takes; the exact size
// of what is sent is got when the title is checked (dl_send_check).
uint64_t dl_item_size(int item);

// ---------------------------------------------------------------------------------------------
// The wireless
// ---------------------------------------------------------------------------------------------

// Opens the local wireless (udsInit). 0 = open; otherwise the result code (usually: the Wi-Fi is off).
int dl_net_open(uint32_t* result);
void dl_net_close(void);
// The radio channel of the room the console has made (1, 6 or 11: the firmware picks it), 0 when there is none yet. One call to the wireless service: make it
// from the thread that makes the room, not from the screen's.
int dl_wifi_channel(void);
// Nothing more to do with the wireless for now: gives back what the module held (the screens' backlight, the apt hook).
void dl_power_exit(void);
// 1 when there is nothing to draw: the backlights were put out with SELECT, or the shell is closed.
int dl_skip_drawing(void);
void dl_toggle_screens(void);
int dl_screens_off(void);
// 1 while the app should go on; 0 when the system asks it to close (HOME menu, Close Software).
int dl_system_ok(void);

// What 3DS Link Delivery (the Download Play host, dlph) borrows from the module while its room is up. The module's own sender and receiver do both
// by themselves; the host library has no module code in it, so the page asks for them.
// The module's log (3DSLink-TitleDelivery.log, written only while the app's Debug log is on): open it before the library, close it after the
// library is closed. dl_log_line is safe from any thread.
void dl_log_open(const char* role);
void dl_log_line(const char* line);
void dl_log_close(void);
// Sleep refused and HOME off, the shell watched (a closed lid puts the screens out and dl_skip_drawing says so): begin after the wireless is open,
// end before it is closed (dl_power_exit, which the app's Wireless does when it closes, gives the rest back).
void dl_power_begin(void);
void dl_power_end(void);

// The app's own CIA, read from the title that is installed on the SD card, the way Titles Delivery's sender reads any title (the virtual CIA of
// titleshare/common/ciastream.c: the content files decrypted from the SD layer, the standard front made from the installed TMD). It is the third source of the
// package the Download Play child is composed from (pages_dlp.cpp, dlp_prepare.cpp). The reads need the keys the scan's context holds (boot9.bin and this console's
// movable.sed): nothing here makes a key of its own. One title at a time, from one thread (the worker of the preparation); the stream is about 400 KB.
enum {
    DL_CIA_OK = 0,
    DL_CIA_NOT_ATTACHED = -1, // the scan of the titles is not done: there is no context to take the key from
    DL_CIA_NO_KEYS = -2,      // the context has no keys (no boot9.bin, no movable.sed, or the key could not be made)
    DL_CIA_NO_TITLE = -3,     // the title is not installed on the SD card (the app was started as a 3dsx): no folder of its contents
    DL_CIA_NOMEM = -4,
    DL_CIA_ERROR = -5         // the title is there but its virtual CIA could not be made (the module's own words are in `why`, in Italian)
};
// Opens the virtual CIA of `tid` and says how big it is. One that is open is closed first.
int dl_cia_open(uint64_t tid, uint64_t* size, char* why, size_t why_size);
// Reads `len` bytes at `off`. 0 = done.
int dl_cia_read(uint64_t off, uint8_t* buf, size_t len);
void dl_cia_close(void);
// How much memory the heap has free (the module's own figure): the preparation of the child compares it with what the composer needs.
uint32_t dl_heap_free(void);

// ---------------------------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------------------------

// Checks a title and gets its size and its seed. 0 = it can be sent; otherwise `error` says why not.
int dl_send_check(int item, uint64_t* size, char* error, size_t error_size);
// Makes the manifest of the titles that were checked. 0 = ready; -1 = none or too many (more than DL_MAX_TITLES); -2 = a title was not checked.
int dl_send_prepare(const int* items, int count);
int dl_send_title_count(void);
void dl_send_title(int index, dl_title* out);
uint64_t dl_send_total_bytes(void);

// The numbers the screens read, with names. The phases of a sending (dl_send_stats.phase), of a receiving (dl_recv_stats.phase), what became of
// a title (dl_recv_stats.result) and what a receiver does (the state of dl_send_stats.client and of dl_recv_stats.other_state). The receiving ones are
// the module's own (delivery.c checks that).
enum { DL_S_OPEN = 0, DL_S_SENDING, DL_S_CLOSED, DL_S_ERROR };
enum { DL_R_SCAN = 0, DL_R_CONNECTING, DL_R_WAIT_LIST, DL_R_CHOOSE, DL_R_WAIT_START, DL_R_RECEIVING, DL_R_DONE, DL_R_ERROR };
enum { DL_RES_PENDING = 0, DL_RES_INSTALLED, DL_RES_SKIPPED, DL_RES_ERROR };
enum { DL_C_DOWNLOADING = 0, DL_C_INSTALLING, DL_C_FINISHED, DL_C_SILENT, DL_C_JOINED, DL_C_WAITING, DL_C_ERROR };
// The space a receiver says it has when it has not said (dl_send_stats.client.free_mib).
#define DL_FREE_UNKNOWN 0xFFFFFFFFu

typedef struct {
    int phase;              // DL_S_*: the room is open, sending, closed, error
    int channel;            // the Wi-Fi channel of the room (1, 6 or 11), 0 = not known
    int connected;          // consoles on the network
    uint32_t next_blocks;   // blocks sent for the first time
    uint64_t repairs;       // blocks sent again
    uint64_t sent_bytes;    // everything the radio sent, new blocks and repeated ones, in bytes
    double mibps;           // speed on the new blocks
    int cur_title;          // the title being sent (-1 none)
    int n_clients;
    struct {
        int down;           // 0..100
        int inst;           // 0..100
        int state;          // DL_C_*: downloading, installing only, finished, not answering, just came in, waiting for its turn, finished with errors
        unsigned age_ds;    // tenths of a second since the sender last heard it
        char name[32];      // the name of the console (its user name on the wireless), "" when not known
        uint8_t want[(DL_MAX_TITLES + 7) / 8]; // the titles it asked for (bit i = title i of the offer)
        uint8_t t_down[DL_MAX_TITLES];         // per title: how much it has, 0..100
        uint8_t t_inst[DL_MAX_TITLES];         // per title: how much is installed, 0..100
        uint32_t free_mib;  // the space it said it has (DL_FREE_UNKNOWN = not said)
        int eff_pct;        // how much of what is sent for it gets there, 0..100
        uint64_t down_bytes; // what it has downloaded of the titles it asked for, in bytes (the speed is the difference over time)
        uint64_t inst_bytes; // what it has installed of them
    } client[DL_MAX_CLIENTS];
    char msg[96];
} dl_send_stats;

typedef int (*dl_send_tick_fn)(void* user, const dl_send_stats* stats);
// Opens the room and keeps it open until the tick answers DL_CANCEL (0), the system closes the app (0) or the room cannot go on (-1).
int dl_send_run(dl_send_tick_fn tick, void* user);
// What the app worked out for the whole room: how much is done (0..100) and the seconds that are left (-1 = nothing to say). The receivers get it
// with the next summary frame, so that what they show is what the sender shows. Call it from the tick.
void dl_send_estimate(int pct, double eta_sec);

// ---------------------------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------------------------

// Looks for rooms in a thread of its own until dl_scan_stop (a scan takes more than a second and cannot be interrupted).
int dl_scan_start(void);
void dl_scan_stop(void);
// How many rooms were seen by the last scan, and what each one says.
typedef struct {
    char host[32];
    unsigned titles;
    uint64_t bytes;
    char first[DL_NAME_BYTES]; // the name of the first title: the whole name when the sender tells it, else its first 15 characters and "..."
    int channel;               // the Wi-Fi channel of the room (1, 6 or 11), 0 = not known
    int receivers, max_receivers; // the receivers that are in the room now, and how many it takes (0 = not known)
} dl_room;
int dl_scan_count(void);
int dl_scan_get(int index, dl_room* out);
// The number of scans finished so far: changes when the list may have changed.
unsigned dl_scan_generation(void);
// Takes the room `index` of the list as it is NOW and keeps a copy of it (the scan goes on until it is stopped and may make a new list: the room that
// is joined is this copy, not whatever is at that place later). 0 = picked, `out` says what it is; -1 = there is no such room.
int dl_scan_pick(int index, dl_room* out);

typedef struct {
    int phase;              // DL_R_*: scan, connecting, waiting for the list, choosing, waiting to start, receiving, done, error
    int channel;            // the Wi-Fi channel of the room (1, 6 or 11), 0 = not known
    int room_full;          // 1 = looking for the room again, it is there but full (room_receivers of room_max_receivers)
    int room_receivers, room_max_receivers;
    uint32_t free_mib;      // the free space of the card as the module read it (DL_FREE_UNKNOWN = not read yet)
    uint64_t cur_inst_bytes; // of the title that is being installed (cur_title): the bytes AM has been given
    int have_manifest;
    int count;              // the titles the sender offers
    dl_title title[DL_MAX_TITLES];
    uint8_t exists[DL_MAX_TITLES];     // already installed on this console
    uint64_t old_size[DL_MAX_TITLES];  // the space it takes now (freed when it is written over)
    uint8_t n3ds_only[DL_MAX_TITLES];  // New 3DS only, and this is an Old 3DS
    uint8_t want[(DL_MAX_TITLES + 7) / 8]; // bit i = title i is chosen: the screen changes it while choosing
    uint8_t result[DL_MAX_TITLES];     // DL_RES_*: waiting, installed, not asked for, error
    uint32_t err[DL_MAX_TITLES];
    int cur_title;
    double mibps;
    uint64_t recv, dups, out_of_window;
    uint32_t am_last_ms, am_max_ms;
    int reconnects;
    uint32_t need_blocks, dl_blocks, inst_blocks;
    double eta_sec;         // -1 = not known
    double sender_eta_sec;
    int all_done;
    uint32_t sender_next;   // how much of the room is done, 0..100
    uint32_t sender_kibs;
    int sender_hears;       // 1 yes, 0 no, -1 not known
    uint32_t data_age_ms;
    int n_others;
    uint32_t other_cursor[16];
    char other_name[16][32];  // the name of each other receiver's console ("" when it is not known)
    uint8_t other_state[16];  // what it does (DL_C_*; 255 = not known)
    char msg[96];
} dl_recv_stats;

typedef int (*dl_recv_tick_fn)(void* user, dl_recv_stats* stats);
// Joins the room that dl_scan_pick took (the scan must be stopped), lets the tick choose and then receives. 0 = all received; 1 = something
// failed or it was cancelled; -1 = it could not join (or no room was picked: the tick is not called then).
int dl_recv_run(dl_recv_tick_fn tick, void* user);

// Is the title in the bitmap `bits` chosen?
int dl_bit(const uint8_t* bits, int i);
void dl_set_bit(uint8_t* bits, int i, int on);

#ifdef __cplusplus
}
#endif
