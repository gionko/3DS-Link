// The bridge between 3DS Link's Titles Delivery screens and the delivery module: see delivery.h. Plain C, the module's headers are included here
// and nowhere else in the app. Nothing here decides anything for the module: it calls the module's functions and copies what it reports.
#include "delivery.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "ciastream.h"
#include "ctx.h"
#include "log.h"
#include "net.h"
#include "power.h"
#include "receiver.h"
#include "sender.h"

// The app's structs (delivery.h) and the module's (sender.h, receiver.h) have arrays that are copied one into the other: they must be the same size.
_Static_assert(DL_MAX_TITLES == TS_MAX_TITLES, "the app and the module disagree on how many titles a room carries");
_Static_assert(DL_MAX_CLIENTS == TS_MAX_CLIENTS, "the app and the module disagree on how many receivers a room serves");
_Static_assert(sizeof(((dl_send_stats*) 0)->client[0].want) == TS_BITMAP_BYTES, "the want bitmaps differ in size");
#define SAME_NUMBER(app, module) ((int) (app) == (int) (module))
_Static_assert(SAME_NUMBER(DL_R_SCAN, TS_R_SCAN) && SAME_NUMBER(DL_R_CONNECTING, TS_R_CONNECT) && SAME_NUMBER(DL_R_WAIT_LIST, TS_R_WAIT_MANIFEST) &&
                   SAME_NUMBER(DL_R_CHOOSE, TS_R_CHOOSE) && SAME_NUMBER(DL_R_WAIT_START, TS_R_WAIT_START) && SAME_NUMBER(DL_R_RECEIVING, TS_R_RECEIVE) &&
                   SAME_NUMBER(DL_R_DONE, TS_R_DONE) && SAME_NUMBER(DL_R_ERROR, TS_R_ERROR),
               "the app and the module disagree on the phases of a receiving");
_Static_assert(SAME_NUMBER(DL_RES_PENDING, TS_RES_PENDING) && SAME_NUMBER(DL_RES_INSTALLED, TS_RES_OK) && SAME_NUMBER(DL_RES_SKIPPED, TS_RES_SKIPPED) &&
                   SAME_NUMBER(DL_RES_ERROR, TS_RES_ERROR),
               "the app and the module disagree on what became of a title");

static ts_ctx* gCtx;
static ts_catalog* gCat;

#ifdef AZL_TEST_DELIVERY_DEMO
// A build made to look at the screens on an emulator: see delivery_demo.inc (included at the end).
static int demo_send_run(dl_send_tick_fn tick, void* user);
static int demo_recv_run(int index, dl_recv_tick_fn tick, void* user);
static int demo_scan_count(void);
static int demo_scan_get(int index, dl_room* out);
#endif

// ---------------------------------------------------------------- the module's state: what the app's scan made

// What the user has checked for sending (0 not checked, 1 can be sent): per title of the catalog.
static uint8_t* gChk;
static ts_title* gInfo;
static int gChkN;

// The installed size of every title of the catalog, from the system (asked once, the first time it is needed).
static uint64_t* gSizes;
static int gSizesN;

static void forget_checks(void) {
    free(gChk);
    free(gInfo);
    gChk = NULL;
    gInfo = NULL;
    gChkN = 0;
    free(gSizes);
    gSizes = NULL;
    gSizesN = 0;
}

void dl_attach(void* ctx, void* catalog) {
    if(gCtx != ctx || gCat != catalog) {
        forget_checks();
    }

    gCtx = (ts_ctx*) ctx;
    gCat = (ts_catalog*) catalog;
}

static int attached(void) {
    return gCtx != NULL && gCat != NULL;
}

int dl_have_keys(void) {
#ifdef AZL_TEST_DELIVERY_DEMO
    return 1; // (a build made to look at the screens on an emulator: see the end of this file)
#endif
    return gCtx != NULL && gCtx->haveKeys;
}

int dl_is_new_3ds(void) {
    return gCtx != NULL && gCtx->isNew;
}

int dl_game_count(void) {
    return gCat != NULL ? gCat->nGames : 0;
}

const char* dl_game_name(int game) {
    return gCat != NULL && game >= 0 && game < gCat->nGames ? gCat->games[game].name : "";
}

int dl_game_member_count(int game) {
    return gCat != NULL && game >= 0 && game < gCat->nGames ? gCat->games[game].nMembers : 0;
}

int dl_game_member(int game, int k) {
    if(gCat == NULL || game < 0 || game >= gCat->nGames || k < 0 || k >= gCat->games[game].nMembers) {
        return -1;
    }

    return gCat->games[game].members[k];
}

int dl_item_count(void) {
    return gCat != NULL ? gCat->nItems : 0;
}

uint64_t dl_item_size(int item) {
    if(gCat == NULL || item < 0 || item >= gCat->nItems) {
        return 0;
    }

    if(gSizes == NULL || gSizesN != gCat->nItems) {
        free(gSizes);
        gSizesN = gCat->nItems;
        gSizes = calloc((size_t) (gSizesN > 0 ? gSizesN : 1), sizeof(uint64_t));
        if(gSizes == NULL) {
            gSizesN = 0;
            return 0;
        }

#ifdef AZL_TEST_DELIVERY_DEMO
        for(int i = 0; i < gSizesN; i++) {
            gSizes[i] = (uint64_t) (180 + 230 * (i % 5)) * 1048576ULL + (uint64_t) i * 4096;
        }
#else
        u64* ids = calloc((size_t) gSizesN, sizeof(u64));
        AM_TitleEntry* infos = calloc((size_t) gSizesN, sizeof(AM_TitleEntry));
        if(ids != NULL && infos != NULL) {
            for(int i = 0; i < gSizesN; i++) {
                ids[i] = gCat->items[i].tid;
            }

            if(R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, (u32) gSizesN, ids, infos))) {
                for(int i = 0; i < gSizesN; i++) {
                    gSizes[i] = infos[i].size;
                }
            } else {
                // one title the system cannot describe makes it refuse the whole list: the others are asked one by one (the broken one stays 0)
                for(int i = 0; i < gSizesN; i++) {
                    gSizes[i] = R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &ids[i], &infos[i])) ? infos[i].size : 0;
                }
            }
        }

        free(ids);
        free(infos);
#endif
    }

    return gSizes[item];
}

uint64_t dl_item_tid(int item) {
    return gCat != NULL && item >= 0 && item < gCat->nItems ? gCat->items[item].tid : 0;
}

int dl_item_kind(int item) {
    return gCat != NULL && item >= 0 && item < gCat->nItems ? gCat->items[item].kind : DL_KIND_OTHER;
}

const char* dl_item_code(int item) {
    return gCat != NULL && item >= 0 && item < gCat->nItems ? gCat->items[item].code : "";
}

const char* dl_item_name(int item) {
    return gCat != NULL && item >= 0 && item < gCat->nItems ? gCat->items[item].name : "";
}

// ---------------------------------------------------------------- the wireless

static int gUdsOk;

int dl_net_open(uint32_t* result) {
#ifdef AZL_TEST_DELIVERY_DEMO
    if(result != NULL) {
        *result = 0;
    }

    return 0;
#endif
    Result r = udsInit(TS_SHARED_MEM, NULL);
    gUdsOk = R_SUCCEEDED(r);
    if(result != NULL) {
        *result = (uint32_t) r;
    }

    return gUdsOk ? 0 : 1;
}

int dl_wifi_channel(void) {
#ifdef AZL_TEST_DELIVERY_DEMO
    return 6;
#endif
    u8 channel = 0;
    if(!gUdsOk || R_FAILED(udsGetChannel(&channel))) {
        return 0;
    }

    return channel;
}

void dl_net_close(void) {
    if(gUdsOk) {
        udsExit();
        gUdsOk = 0;
    }
}

void dl_power_exit(void) {
    ts_power_exit();
}

int dl_skip_drawing(void) {
    return ts_power_skip_drawing();
}

void dl_toggle_screens(void) {
    ts_power_toggle_screens();
}

int dl_screens_off(void) {
    return ts_power_screens_off();
}

int dl_system_ok(void) {
    return aptMainLoop() ? 1 : 0;
}

void dl_log_open(const char* role) {
    ts_log_open(role);
}

void dl_log_line(const char* line) {
    ts_log("%s", line);
}

void dl_log_close(void) {
    ts_log_close();
}

void dl_power_begin(void) {
    ts_power_begin_transfer();
}

void dl_power_end(void) {
    ts_power_end_transfer();
}

uint32_t dl_heap_free(void) {
    return ts_mem_heap_free();
}

// ---------------------------------------------------------------- the app's own CIA, for the composer of the Download Play child

// The virtual CIA of one installed title, kept between dl_cia_open and dl_cia_close (about 400 KB: made on the heap, not on a stack).
static cia_stream* gAppCia;

void dl_cia_close(void) {
    if(gAppCia != NULL) {
        cia_stream_close(gAppCia);
        free(gAppCia);
        gAppCia = NULL;
    }
}

int dl_cia_open(uint64_t tid, uint64_t* size, char* why, size_t why_size) {
    dl_cia_close();
    if(why != NULL && why_size > 0) {
        why[0] = 0;
    }

#ifdef AZL_TEST_DELIVERY_DEMO
    (void) tid;
    (void) size;
    return DL_CIA_NO_KEYS; // (a build made to look at the screens on an emulator: no title, no keys)
#endif
    if(gCtx == NULL) {
        return DL_CIA_NOT_ATTACHED;
    }

    if(!gCtx->haveKeys || gCtx->ctrRoot[0] == 0 || gCtx->certsDb == NULL) {
        return DL_CIA_NO_KEYS;
    }

    gAppCia = calloc(1, sizeof(cia_stream));
    if(gAppCia == NULL) {
        return DL_CIA_NOMEM;
    }

    int rc = cia_stream_open(gAppCia, tid, gCtx->ctrRoot, gCtx->rk, gCtx->certsDb, gCtx->certsDbSize);
    if(rc != 0) {
        if(why != NULL && why_size > 0) {
            snprintf(why, why_size, "%s", gAppCia->err);
        }

        dl_cia_close();
        return rc == -1 ? DL_CIA_NO_TITLE : DL_CIA_ERROR; // (-1: the folder of the title's contents is not there: it is not installed)
    }

    if(size != NULL) {
        *size = gAppCia->front.total;
    }

    return DL_CIA_OK;
}

int dl_cia_read(uint64_t off, uint8_t* buf, size_t len) {
    return gAppCia != NULL ? cia_stream_read(gAppCia, off, buf, len) : -1;
}

int dl_bit(const uint8_t* bits, int i) {
    return ts_bit(bits, i);
}

void dl_set_bit(uint8_t* bits, int i, int on) {
    ts_set_bit(bits, i, on);
}

static void fill_title(dl_title* out, const ts_title* t) {
    memset(out, 0, sizeof(*out));
    out->tid = t->tid;
    out->size = t->size;
    out->kind = t->kind;
    out->has_seed = t->hasSeed;
    snprintf(out->code, sizeof(out->code), "%s", t->code);
    snprintf(out->name, sizeof(out->name), "%s", t->name);
}

// ---------------------------------------------------------------- what the module says

// The module's messages (what it tells its user while it works) are in Italian, as is its log. The screens say them in English: a message
// the table does not know is shown as the module wrote it.
static void english_msg(const char* in, char* out, size_t size) {
    static const struct {
        const char* it;
        const char* en;
    } table[] = {
        {"mi collego", "connecting..."},
        {"attendo l'elenco", "waiting for the list of titles..."},
        {"il sistema chiede di chiudere", "the system asks the app to close"},
        {"il mittente e' ripartito", "the sender started again: join its room again"},
        {"scegli cosa ricevere", "choose what to receive"},
        {"in attesa: il mittente sta servendo altri", "waiting: the sender is serving others"},
        {"tutto scaricato: installo", "all downloaded: installing"},
        {"il mittente ha chiuso la stanza", "the sender closed the room"},
        {"installazione completata", "installation complete"},
        {"finito con errori", "finished with errors"},
        {"mittente non ritrovato", "the sender was not found again"},
        {"annullato", "cancelled"},
        {"stanza aperta: aspetto i riceventi", "room open: waiting for receivers"},
        {"sala chiusa", "room closed"},
    };
    for(size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if(strstr(in, table[i].it) == in) {
            snprintf(out, size, "%s", table[i].en);
            return;
        }
    }

    const char* tail = strchr(in, '(');
    if(strstr(in, "collegamento fallito") == in) {
        snprintf(out, size, "could not join the room %s", tail != NULL ? tail : "");
    } else if(strstr(in, "rete non creata") == in) {
        snprintf(out, size, "could not create the network %s", tail != NULL ? tail : "");
    } else if(strstr(in, "memoria insufficiente") == in) {
        snprintf(out, size, "not enough memory");
    } else if(strstr(in, "thread ") == in) {
        snprintf(out, size, "a thread could not be started");
    } else if(strstr(in, "troppi errori di invio") == in) {
        snprintf(out, size, "too many errors while sending %s", tail != NULL ? tail : "");
    } else if(strstr(in, "lettura: ") == in) {
        snprintf(out, size, "read error: %s", in + 9);
    } else if(strstr(in, "stanza piena") == in) {
        // "stanza piena (7/7): riprovo (N s) B = rinuncia": the room is there but takes nobody else
        int inRoom = 0, maxRoom = 0;
        unsigned secs = 0;
        const char* counts = strchr(in, '(');
        const char* time_part = counts != NULL ? strchr(counts + 1, '(') : NULL;
        if(counts != NULL && time_part != NULL && sscanf(counts, "(%d/%d)", &inRoom, &maxRoom) == 2 && sscanf(time_part, "(%u s)", &secs) == 1) {
            if(secs >= 60) {
                snprintf(out, size, "The room is full (%d/%d), still trying (%u min %02u s)", inRoom, maxRoom, secs / 60, secs % 60);
            } else {
                snprintf(out, size, "The room is full (%d/%d), still trying (%u s)", inRoom, maxRoom, secs);
            }
        } else {
            snprintf(out, size, "The room is full, still trying");
        }
    } else if(strstr(in, "collegamento perso") == in) {
        unsigned secs = 0;
        if(tail != NULL && sscanf(tail, "(%u s)", &secs) == 1) {
            // the search goes on for 30 minutes at most: after the first minute the screen says how far it has got
            if(secs >= 60) {
                snprintf(out, size, "connection lost: looking for the sender... (%u min %02u s of 30)", secs / 60, secs % 60);
            } else {
                snprintf(out, size, "connection lost: looking for the sender... (%u s)", secs);
            }
        } else {
            snprintf(out, size, "connection lost: looking for the sender...");
        }
    } else if(strstr(in, "ricezione in corso") == in) {
        snprintf(out, size, "receiving");
    } else {
        snprintf(out, size, "%s", in);
    }
}

// ---------------------------------------------------------------- sending

// The module's reasons are in Italian (they go to its log too): the ones the user can meet are told in English here.
static void english_reason(const char* module_text, char* out, size_t size) {
    if(strstr(module_text, "chiavi") != NULL) {
        snprintf(out, size, "the keys are not available");
    } else if(strstr(module_text, "4 GiB") != NULL) {
        snprintf(out, size, "over 4 GiB (the FAT32 limit)");
    } else if(strstr(module_text, "non leggibile") != NULL) {
        snprintf(out, size, "its files cannot be read");
    } else {
        snprintf(out, size, "%s", module_text[0] ? module_text : "it cannot be read");
    }
}

int dl_send_check(int item, uint64_t* size, char* error, size_t error_size) {
    error[0] = 0;
    if(!attached() || item < 0 || item >= gCat->nItems) {
        snprintf(error, error_size, "it is not in the list");
        return -1;
    }

    if(gChk == NULL || gChkN != gCat->nItems) {
        forget_checks();
        gChk = calloc((size_t) (gCat->nItems > 0 ? gCat->nItems : 1), 1);
        gInfo = calloc((size_t) (gCat->nItems > 0 ? gCat->nItems : 1), sizeof(ts_title));
        gChkN = gCat->nItems;
        if(gChk == NULL || gInfo == NULL) {
            forget_checks();
            snprintf(error, error_size, "not enough memory");
            return -1;
        }
    }

    if(gChk[item] != 1) {
        ts_title t;
        char why[96];
#ifdef AZL_TEST_DELIVERY_DEMO
        memset(&t, 0, sizeof(t));
        t.tid = gCat->items[item].tid;
        t.size = (uint64_t) (180 + 230 * (item % 5)) * 1048576ULL + (uint64_t) item * 4096;
        t.hasSeed = item % 4 == 1;
        snprintf(t.code, sizeof(t.code), "%s", gCat->items[item].code);
        why[0] = 0;
        if(0) {
#else
        if(sender_check_title(gCtx, gCat->items[item].tid, &t, why, sizeof(why)) != 0) {
#endif
            english_reason(why, error, error_size);
            return -1;
        }

        snprintf(t.name, sizeof(t.name), "%s", gCat->items[item].name);
        t.kind = gCat->items[item].kind;
        gInfo[item] = t;
        gChk[item] = 1;
    }

    if(size != NULL) {
        *size = gInfo[item].size;
    }

    return 0;
}

static ts_manifest gMan;

int dl_send_prepare(const int* items, int count) {
    if(count < 1 || count > TS_MAX_TITLES) {
        return -1;
    }

    ts_title list[TS_MAX_TITLES];
    for(int i = 0; i < count; i++) {
        if(gChk == NULL || items[i] < 0 || items[i] >= gChkN || gChk[items[i]] != 1) {
            return -2;
        }

        list[i] = gInfo[items[i]];
    }

    return sender_make_manifest(list, count, &gMan) == 0 ? 0 : -1;
}

int dl_send_title_count(void) {
    return gMan.count;
}

void dl_send_title(int index, dl_title* out) {
    if(index < 0 || index >= gMan.count) {
        memset(out, 0, sizeof(*out));
        return;
    }

    fill_title(out, &gMan.t[index]);
}

uint64_t dl_send_total_bytes(void) {
    uint64_t total = 0;
    for(int i = 0; i < gMan.count; i++) {
        total += gMan.t[i].size;
    }

    return total;
}

static dl_send_tick_fn gSendTick;
static void* gSendUser;

static int send_adapter(void* arg, const ts_sender_stats* st) {
    (void) arg;
    static dl_send_stats s; // (static: the stack of a callback that runs inside the module's loops stays small)
    memset(&s, 0, sizeof(s));
    s.phase = st->phase;
    s.channel = st->channel;
    s.connected = st->connected;
    s.next_blocks = st->next;
    s.repairs = st->repairs;
    s.sent_bytes = ((uint64_t) st->next + st->repairs) * TS_BLOCK;
    s.mibps = st->mibps;
    s.cur_title = st->curTitle;
    s.n_clients = st->nClients < DL_MAX_CLIENTS ? st->nClients : DL_MAX_CLIENTS;
    for(int i = 0; i < s.n_clients; i++) {
        s.client[i].down = st->pctDown[i];
        s.client[i].inst = st->pctInst[i];
        s.client[i].state = st->cstate[i];
        s.client[i].age_ds = st->ageDs[i];
        snprintf(s.client[i].name, sizeof(s.client[i].name), "%s", st->cname[i]);
        memcpy(s.client[i].want, st->cwant[i], sizeof(s.client[i].want));
        memcpy(s.client[i].t_down, st->cTitleDown[i], sizeof(s.client[i].t_down));
        memcpy(s.client[i].t_inst, st->cTitleInst[i], sizeof(s.client[i].t_inst));
        s.client[i].free_mib = st->cFreeMiB[i];
        s.client[i].eff_pct = st->cEff[i] / 10;
        s.client[i].down_bytes = (uint64_t) st->cDownBlocks[i] * TS_BLOCK;
        s.client[i].inst_bytes = (uint64_t) st->cInstBlocks[i] * TS_BLOCK;
    }

    english_msg(st->msg, s.msg, sizeof(s.msg));
    int answer = gSendTick(gSendUser, &s);
    // (the module only lets the room be closed from its first two phases)
    return answer == DL_CANCEL && st->phase <= 1 ? TS_UI_CANCEL : TS_UI_NONE;
}

void dl_send_estimate(int pct, double eta_sec) {
#ifndef AZL_TEST_DELIVERY_DEMO
    sender_set_room_estimate(pct, eta_sec);
#else
    (void) pct;
    (void) eta_sec;
#endif
}

int dl_send_run(dl_send_tick_fn tick, void* user) {
#ifdef AZL_TEST_DELIVERY_DEMO
    return demo_send_run(tick, user);
#endif
    gSendTick = tick;
    gSendUser = user;
    return sender_run(gCtx, &gMan, send_adapter, NULL);
}

// ---------------------------------------------------------------- receiving: the rooms around

static Thread gScanThread;
static volatile int gScanStop;
static LightLock gScanLock;
static int gScanLockReady;
static udsNetworkScanInfo* gNets;
static size_t gNetsN;
static unsigned gScanGeneration;
static udsNetworkScanInfo gPicked; // the room dl_scan_pick took
static int gPickedOk;

static void scan_main(void* arg) {
    (void) arg;
    while(!gScanStop) {
        udsNetworkScanInfo* nets = NULL;
        size_t total = 0;
        if(receiver_scan(&nets, &total) != 0) {
            nets = NULL;
            total = 0;
        }

        if(gScanStop) {
            free(nets); // (the scan was stopped while this pass was running: its list is of no use to anyone)
            break;
        }

        LightLock_Lock(&gScanLock);
        free(gNets);
        gNets = nets;
        gNetsN = total;
        gScanGeneration++;
        LightLock_Unlock(&gScanLock);
        for(int i = 0; i < 15 && !gScanStop; i++) {
            svcSleepThread(100ULL * 1000000ULL);
        }
    }
}

int dl_scan_start(void) {
#ifdef AZL_TEST_DELIVERY_DEMO
    return 0;
#endif
    if(!gScanLockReady) {
        LightLock_Init(&gScanLock);
        gScanLockReady = 1;
    }

    if(gScanThread != NULL) {
        return 0;
    }

    // The rooms of an earlier visit are not rooms any more: the list starts empty and the first pass fills it.
    LightLock_Lock(&gScanLock);
    free(gNets);
    gNets = NULL;
    gNetsN = 0;
    gScanGeneration++;
    LightLock_Unlock(&gScanLock);
    gPickedOk = 0;
    gScanStop = 0;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    gScanThread = threadCreate(scan_main, NULL, 32 * 1024, priority + 1, -2, false);
    return gScanThread != NULL ? 0 : -1;
}

void dl_scan_stop(void) {
    if(gScanThread == NULL) {
        return;
    }

    gScanStop = 1;
    threadJoin(gScanThread, U64_MAX);
    threadFree(gScanThread);
    gScanThread = NULL;
}

int dl_scan_count(void) {
#ifdef AZL_TEST_DELIVERY_DEMO
    return demo_scan_count();
#endif
    if(!gScanLockReady) {
        return 0;
    }

    LightLock_Lock(&gScanLock);
    int n = (int) gNetsN;
    LightLock_Unlock(&gScanLock);
    return n;
}

int dl_scan_get(int index, dl_room* out) {
#ifdef AZL_TEST_DELIVERY_DEMO
    return demo_scan_get(index, out);
#endif
    memset(out, 0, sizeof(*out));
    if(!gScanLockReady) {
        return -1;
    }

    LightLock_Lock(&gScanLock);
    int ok = index >= 0 && (size_t) index < gNetsN;
    if(ok) {
        uint8_t titles = 0;
        uint64_t bytes = 0;
        receiver_describe(&gNets[index], out->host, sizeof(out->host), &titles, &bytes, out->first, sizeof(out->first), &out->channel, &out->receivers, &out->max_receivers);
        out->titles = titles;
        out->bytes = bytes;
    }

    LightLock_Unlock(&gScanLock);
    return ok ? 0 : -1;
}

int dl_scan_pick(int index, dl_room* out) {
#ifdef AZL_TEST_DELIVERY_DEMO
    gPickedOk = demo_scan_get(index, out) == 0;
    return gPickedOk ? 0 : -1;
#endif
    memset(out, 0, sizeof(*out));
    gPickedOk = 0;
    if(!gScanLockReady) {
        return -1;
    }

    LightLock_Lock(&gScanLock);
    int ok = index >= 0 && (size_t) index < gNetsN;
    if(ok) {
        memcpy(&gPicked, &gNets[index], sizeof(gPicked));
        uint8_t titles = 0;
        uint64_t bytes = 0;
        receiver_describe(&gPicked, out->host, sizeof(out->host), &titles, &bytes, out->first, sizeof(out->first), &out->channel, &out->receivers, &out->max_receivers);
        out->titles = titles;
        out->bytes = bytes;
        gPickedOk = 1;
    }

    LightLock_Unlock(&gScanLock);
    return ok ? 0 : -1;
}

unsigned dl_scan_generation(void) {
#ifdef AZL_TEST_DELIVERY_DEMO
    return (unsigned) (ts_now_ms() / 1500);
#endif
    return gScanGeneration;
}

// ---------------------------------------------------------------- receiving: the room

static dl_recv_tick_fn gRecvTick;
static void* gRecvUser;

// The name of the console that speaks from a node of the wireless network (what the console calls itself in its settings), kept for two seconds
// (the answer comes from the system service: not asked a hundred times a second). "" when the node is not known.
static char gNodeNames[32][32];
static uint64_t gNodeNameAt[32];

static void node_name(uint8_t node, char* out, size_t size, int may_ask) {
    out[0] = 0;
    if(node == 0 || node >= 32) {
        return;
    }

    const uint64_t now = ts_now_ms();
    // (the question goes to the wireless service, the one the search for the room uses: while it searches the screen would wait for it, so it keeps the names it has)
    if(may_ask && (gNodeNameAt[node] == 0 || now - gNodeNameAt[node] > 2000)) {
        gNodeNameAt[node] = now;
        udsNodeInfo ni;
        char un[32];
        memset(un, 0, sizeof(un));
        // the last good name stays: a lookup that fails for a moment must not turn it into "Receiver N" (the sender only tells a node that is in the network)
        if(R_SUCCEEDED(udsGetNodeInformation(node, &ni)) && R_SUCCEEDED(udsGetNodeInfoUsername(&ni, un)) && un[0] != 0) {
            snprintf(gNodeNames[node], sizeof(gNodeNames[node]), "%s", un);
        }
    }

    snprintf(out, size, "%s", gNodeNames[node]);
}

static int recv_adapter(void* arg, ts_recv_stats* st) {
    (void) arg;
    static dl_recv_stats s;
    memset(&s, 0, sizeof(s));
    s.phase = st->phase;
    s.channel = st->channel;
    s.room_full = st->roomFull;
    s.room_receivers = st->roomNodes;
    s.room_max_receivers = st->roomMax;
    s.free_mib = st->freeMiB;
    s.cur_inst_bytes = st->curInstBytes;
    s.have_manifest = st->haveManifest;
    s.count = st->haveManifest ? st->m.count : 0;
    if(s.count > DL_MAX_TITLES) {
        s.count = DL_MAX_TITLES;
    }

    for(int i = 0; i < s.count; i++) {
        fill_title(&s.title[i], &st->m.t[i]);
        s.exists[i] = st->exists[i];
        s.old_size[i] = st->oldSize[i];
        s.n3ds_only[i] = st->n3dsOnly[i];
        s.result[i] = st->result[i];
        s.err[i] = st->err[i];
    }

    memcpy(s.want, st->want, sizeof(s.want));
    s.cur_title = st->curTitle;
    s.mibps = st->mibps;
    s.recv = st->recv;
    s.dups = st->dups;
    s.out_of_window = st->outOfWindow;
    s.am_last_ms = st->amLastMs;
    s.am_max_ms = st->amMaxMs;
    s.reconnects = st->reconnects;
    s.need_blocks = st->needBlocks;
    s.dl_blocks = st->dlBlocks;
    s.inst_blocks = st->instBlocks;
    s.eta_sec = st->etaSec;
    s.sender_eta_sec = st->senderEtaSec;
    s.all_done = st->allDone;
    s.sender_next = st->senderNext;
    s.sender_kibs = st->senderKiBs;
    s.sender_hears = st->senderHears;
    s.data_age_ms = st->dataAgeMs;
    s.n_others = st->nOthers < 16 ? st->nOthers : 16;
    for(int i = 0; i < s.n_others; i++) {
        s.other_cursor[i] = st->otherCursor[i];
        s.other_state[i] = st->otherState[i];
        node_name(st->otherNode[i], s.other_name[i], sizeof(s.other_name[i]), !st->reconnecting);
    }

    english_msg(st->msg, s.msg, sizeof(s.msg));
    int answer = gRecvTick(gRecvUser, &s);
    if(st->phase == TS_R_CHOOSE) {
        memcpy(st->want, s.want, sizeof(st->want)); // (the choice is the screen's)
    }

    if(answer == DL_CANCEL && st->phase != TS_R_DONE && st->phase != TS_R_ERROR) {
        return TS_RUI_CANCEL;
    }

    return answer == DL_CONFIRM ? TS_RUI_CONFIRM : TS_RUI_NONE;
}

int dl_recv_run(dl_recv_tick_fn tick, void* user) {
#ifdef AZL_TEST_DELIVERY_DEMO
    return demo_recv_run(0, tick, user);
#endif
    if(!gPickedOk) {
        return -1;
    }

    gRecvTick = tick;
    gRecvUser = user;
    gPickedOk = 0;
    memset(gNodeNames, 0, sizeof(gNodeNames)); // (the names of an earlier room say nothing about this one)
    memset(gNodeNameAt, 0, sizeof(gNodeNameAt));
    return receiver_run(gCtx, &gPicked, recv_adapter, NULL);
}

#ifdef AZL_TEST_DELIVERY_DEMO
#include "delivery_demo.inc"
#endif
