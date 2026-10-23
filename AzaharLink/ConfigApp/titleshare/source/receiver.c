#include "receiver.h"

#include <dirent.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amsink.h"
#include "catalog.h"
#include "hostcfg.h"
#include "store.h"
#include "log.h"
#include "membudget.h"
#include "net.h"
#include "parkgate.h"
#include "power.h"
#include "win.h"

int receiver_scan(udsNetworkScanInfo** nets, size_t* total) {
    return receiver_scan_host(nets, total, NULL);
}

int receiver_scan_host(udsNetworkScanInfo** nets, size_t* total, const uint8_t* hostMac) {
    uint8_t* buf = malloc(0x4000);
    if(buf == NULL) {
        return -1;
    }

    *nets = NULL;
    *total = 0;
    memset(buf, 0, 0x4000);
    Result r = udsScanBeacons(buf, 0x4000, nets, total, TS_WLANCOMM_ID, 0, hostMac, false);
    free(buf);
    return R_SUCCEEDED(r) ? 0 : -2;
}

void receiver_describe(const udsNetworkScanInfo* net, char* host, size_t hostSize, uint8_t* titles, uint64_t* totalBytes, char* firstName, size_t firstNameSize, int* channel, int* nodes,
                       int* maxNodes) {
    host[0] = 0;
    *titles = 0;
    *totalBytes = 0;
    firstName[0] = 0;
    if(channel != NULL) {
        *channel = (int) net->network.channel;
    }

    if(nodes != NULL) {
        *nodes = net->network.total_nodes > 0 ? (int) net->network.total_nodes - 1 : 0; // (the receivers: the sender is not counted)
    }

    if(maxNodes != NULL) {
        *maxNodes = net->network.max_nodes > 0 ? (int) net->network.max_nodes - 1 : 0;
    }

    char name[32] = {0};
    if(udsCheckNodeInfoInitialized(&net->nodes[0]) && R_SUCCEEDED(udsGetNodeInfoUsername(&net->nodes[0], name))) {
        snprintf(host, hostSize, "%s", name);
    }

    ts_appdata ad;
    size_t actual = 0;
    memset(&ad, 0, sizeof(ad));
    if(R_SUCCEEDED(udsGetNetworkStructApplicationData(&net->network, &ad, sizeof(ad), &actual)) && actual >= TS_APPDATA_OLD_SIZE && ad.magic == TS_APPDATA_MAGIC) {
        *titles = ad.titles;
        *totalBytes = ad.totalBytes;
        ad.firstCode[15] = 0;
        ad.firstName[sizeof(ad.firstName) - 1] = 0;
        if(actual >= sizeof(ad) && ad.firstName[0] != 0) {
            snprintf(firstName, firstNameSize, "%s", ad.firstName); // the whole name
        } else if(strlen(ad.firstCode) >= 15) {
            snprintf(firstName, firstNameSize, "%s...", ad.firstCode); // an older sender: the name was cut at 15 characters
        } else {
            snprintf(firstName, firstNameSize, "%s", ad.firstCode);
        }
    }
}


// ---------------------------------------------------------------- file temporanei di parcheggio (SD)

// la cartella di parcheggio sta dentro la cartella dei dati dell'app (hostcfg.h); si ricava a ogni uso, perche' la sceglie l'app
static int park_dir(char* out, size_t n) {
    return hc_data_path(out, n, "park");
}

// Un file per titolo, un solo handle FS aperto in lettura e scrittura (offset a 64 bit: i file arrivano a 4 GiB). Si scrive sempre in coda;
// l'installazione legge dallo stesso file mentre il parcheggio continua a scrivere.
typedef struct {
    const ts_manifest* m;
    Handle h[TS_MAX_TITLES];
    uint8_t isOpen[TS_MAX_TITLES];
    uint64_t size[TS_MAX_TITLES];
    volatile uint32_t nWrites, nReads, maxWriteMs, maxReadMs;
    // strumentazione (solo registro): letture dal file di parcheggio fatte dall'installatore, ms totali e massimo (cumulativi, mai azzerati) e blocchi letti, per dire
    // 'ms per lettura' e 'blocchi per lettura'. 32 bit: scritti da un solo thread (installazione), letti dal thread del registro.
    volatile uint32_t readMsTotal, readMsMax, readBlocks;
} parkio_t;

static int park_path(const parkio_t* p, int t, char* out, size_t n) {
    char dir[HC_PATH_MAX];
    if(park_dir(dir, sizeof(dir)) != 0) {
        out[0] = 0;
        return -1;
    }

    int len = snprintf(out, n, "%s/%016llx.blk", dir, (unsigned long long) p->m->t[t].tid);
    return (len > 0 && (size_t) len < n) ? 0 : -1;
}

static int pio_open(void* c, int t) {
    parkio_t* p = (parkio_t*) c;
    char dir[HC_PATH_MAX];
    if(park_dir(dir, sizeof(dir)) != 0) {
        ts_log("parcheggio: cartella dei dati non impostata");
        return -1;
    }

    mkdir(hc_data_dir(), 0777);
    mkdir(dir, 0777);
    char path[HC_PATH_MAX];
    if(park_path(p, t, path, sizeof(path)) != 0) {
        return -1;
    }

    remove(path); // un file con lo stesso nome e' un resto di una prova interrotta
    // l'archivio e' la SD; il percorso e' relativo alla sua radice
    char fp[HC_PATH_MAX];
    if(hc_sd_relative(fp, sizeof(fp), path) != 0) {
        return -1;
    }

    Handle h = 0;
    Result r = FSUSER_OpenFileDirectly(&h, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, fp), FS_OPEN_CREATE | FS_OPEN_READ | FS_OPEN_WRITE, 0);
    if(R_FAILED(r)) {
        ts_log("parcheggio: file del titolo %d non creato (0x%08x)", t, (unsigned int) r);
        return -1;
    }

    p->h[t] = h;
    p->isOpen[t] = 1;
    p->size[t] = 0;
    return 0;
}

static int pio_append(void* c, int t, const uint8_t* d, size_t len, uint64_t* off) {
    parkio_t* p = (parkio_t*) c;
    if(!p->isOpen[t]) {
        return -1;
    }

    uint32_t written = 0;
    uint64_t t0 = ts_now_ms();
    Result r = FSFILE_Write(p->h[t], &written, p->size[t], d, (u32) len, 0);
    uint32_t ms = (uint32_t) (ts_now_ms() - t0);
    p->nWrites++;
    if(ms > p->maxWriteMs) {
        p->maxWriteMs = ms;
    }

    if(ms > 1500) {
        ts_log("parcheggio: scrittura lenta %u ms (titolo %d, offset %llu)", (unsigned int) ms, t, (unsigned long long) p->size[t]);
    }

    if(R_FAILED(r) || written != len) {
        ts_log("parcheggio: scrittura fallita 0x%08x (titolo %d, offset %llu, scritti %u di %u)", (unsigned int) r, t, (unsigned long long) p->size[t], (unsigned int) written, (unsigned int) len);
        return -1;
    }

    *off = p->size[t];
    p->size[t] += len;
    return 0;
}

static int pio_read(void* c, int t, uint64_t off, uint8_t* out, size_t len) {
    parkio_t* p = (parkio_t*) c;
    if(!p->isOpen[t]) {
        return -1;
    }

    uint32_t got = 0;
    uint64_t t0 = ts_now_ms();
    Result r = FSFILE_Read(p->h[t], &got, off, out, (u32) len);
    uint32_t ms = (uint32_t) (ts_now_ms() - t0);
    p->nReads++;
    p->readMsTotal += ms;
    p->readBlocks += (uint32_t) ((len + TS_BLOCK - 1) / TS_BLOCK); // divisione per una costante: moltiplicazione, nessuna chiamata; una volta per lettura
    if(ms > p->readMsMax) {
        p->readMsMax = ms;
    }

    if(ms > p->maxReadMs) {
        p->maxReadMs = ms;
    }

    if(R_FAILED(r) || got != len) {
        ts_log("parcheggio: lettura fallita 0x%08x (titolo %d, offset %llu, letti %u di %u)", (unsigned int) r, t, (unsigned long long) off, (unsigned int) got, (unsigned int) len);
        return -1;
    }

    return 0;
}

// Orologio dello store (ms, 32 bit): serve all'eta' delle voci della coda di parcheggio.
static uint32_t pio_now(void* c) {
    (void) c;
    return (uint32_t) ts_now_ms();
}

static void pio_remove(void* c, int t) {
    parkio_t* p = (parkio_t*) c;
    if(p->isOpen[t]) {
        FSFILE_Close(p->h[t]);
        p->isOpen[t] = 0;
        p->h[t] = 0;
    }

    char path[HC_PATH_MAX];
    if(park_path(p, t, path, sizeof(path)) == 0) {
        remove(path);
    }

    p->size[t] = 0;
}

// Cancella i file di parcheggio rimasti da un'esecuzione interrotta.
static void park_cleanup(void) {
    char dir[HC_PATH_MAX];
    if(park_dir(dir, sizeof(dir)) != 0) {
        return;
    }

    DIR* d = opendir(dir);
    if(d == NULL) {
        return;
    }

    struct dirent* e;
    int n = 0;
    while((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if(l > 4 && strcmp(e->d_name + l - 4, ".blk") == 0) {
            char path[HC_PATH_MAX + 208];
            snprintf(path, sizeof(path), "%s/%.200s", dir, e->d_name);
            if(remove(path) == 0) {
                n++;
            }
        }
    }

    closedir(d);
    if(n > 0) {
        ts_log("parcheggio: cancellati %d file temporanei rimasti da un'esecuzione interrotta", n);
    }
}

// ---------------------------------------------------------------- installazione in AM (thread)

typedef struct {
    ts_store* store;
    const ts_manifest* m;
    uint8_t want[TS_BITMAP_BYTES];   // cosa vuole questo ricevente
    uint8_t exists[TS_MAX_TITLES];   // gia' installato: si sovrascrive
    volatile int stop, finished;
    volatile int curTitle;           // titolo in installazione (-1 = nessuno)
    volatile int hintTitle;          // titolo dell'ultimo blocco di dati arrivato (suggerisce quale installare per primo)
    volatile int stateTitle;         // titolo in corso annunciato dal mittente
    volatile uint8_t result[TS_MAX_TITLES];
    volatile uint32_t err[TS_MAX_TITLES];
    volatile uint64_t bytes;         // byte consegnati ad AM
    volatile uint64_t curBytes;      // byte consegnati ad AM del titolo che si sta installando (per "installing N%" sullo schermo)
    volatile uint32_t amLastMs, amMaxMs;
    volatile uint32_t amTotalMs, amWrites; // tempo totale e numero delle scritture verso AM
    volatile uint64_t amBytes;             // byte scritti in AM (per la velocita' di AM mentre scrive)
    volatile uint32_t amHist[5];           // durata delle scritture: <20, 20-50, 50-100, 100-500, >=500 ms
    volatile uint32_t amSizeHist[5];       // dimensione delle scritture in blocchi: <8, 8-15, 16-31, 32-63, >=64
    volatile uint32_t waitMs;              // tempo passato ad aspettare dati (installatore a secco)
    ts_amgate gate;                        // AM sta lavorando (raccolta + scrittura di un gruppo, apertura, chiusura) o ha scritto da meno di 150 ms: il parcheggio non deve contendergli la SD (vedi parkgate.h)
    volatile uint64_t fileBytes, winBytes; // byte consegnati dal file e dalla finestra
    int wd; // id del watchdog dello scrittore (-1 = nessuno)
} writer_t;

// Lettura di un contatore a 64 bit scritto da un altro thread: su ARMv6K un LDRD non e' atomico, si rilegge finche' due letture coincidono.
static inline uint64_t rd64(const volatile uint64_t* p) {
    uint64_t a, b;
    do {
        a = *p;
        b = *p;
    } while(a != b);
    return a;
}

// SD libera in byte (0 se non leggibile).
static uint64_t sd_free_bytes(void) {
    FS_ArchiveResource sd;
    memset(&sd, 0, sizeof(sd));
    if(R_FAILED(FSUSER_GetSdmcArchiveResource(&sd))) {
        return 0;
    }

    return (uint64_t) sd.freeClusters * sd.clusterSize;
}

static double sd_free_gib(void) {
    uint64_t b = sd_free_bytes();
    return b ? (double) b / 1073741824.0 : -1.0;
}

static int wanted_pending(const writer_t* wt) {
    for(int i = 0; i < wt->m->count; i++) {
        if(ts_bit(wt->want, i) && wt->result[i] == TS_RES_PENDING) {
            return 1;
        }
    }

    return 0;
}

// Blocchi per scrittura verso AM (massimo): 128 = 186 KB. Una scrittura costa ~12 ms fissi + ~0,45 ms per blocco; FBI usa 256 KiB.
#define TS_AM_AGG 128

// After a title failed in AM (a write refused, the file of the parked blocks unreadable, the end of the install refused) what AM may have kept of it is taken away: the title, its
// ticket and a pending install, of THAT title only (never the game it belongs to, never another title). Without it a failed DLC stayed on the console (0.4 GiB) and the app
// then called it "already installed".
static void scrub_failed(writer_t* wt, u64 tid) {
    ts_wd_beat(wt->wd, 6);
    ts_amgate_begin(&wt->gate, (uint32_t) ts_now_ms(), TS_AM_STUCK_MS);
    FS_MediaType dest = ts_title_dest(tid);
    Result rt = AM_DeleteTitle(dest, tid);
    Result rk = AM_DeleteTicket(tid);
    Result rp = AM_DeletePendingTitle(dest, tid);
    ts_amgate_end(&wt->gate, (uint32_t) ts_now_ms(), TS_AM_TAIL_MS);
    ts_log("pulizia dopo l'errore di %016llx: DeleteTitle 0x%08x, DeleteTicket 0x%08x, DeletePendingTitle 0x%08x", (unsigned long long) tid, (unsigned int) rt, (unsigned int) rk, (unsigned int) rp);
    ts_wd_beat(wt->wd, 7);
}

static void writer_fn(void* arg) {
    writer_t* wt = (writer_t*) arg;
    ts_store* S = wt->store;
    const ts_manifest* m = wt->m;
    ts_log("installazione: thread avviato sul core %d", (int) svcGetProcessorID());
    uint8_t* stage = malloc((size_t) TS_AM_AGG * TS_BLOCK);
    if(stage == NULL) {
        // senza memoria non si installa nulla: i titoli voluti risultano in errore (altrimenti il ricevente direbbe "completato")
        for(int i = 0; i < m->count; i++) {
            if(ts_bit(wt->want, i)) {
                wt->result[i] = TS_RES_ERROR;
                wt->err[i] = 0xC8604C01;
            }
        }

        wt->finished = 1;
        return;
    }

    int cur = -1;
    Handle h = 0;
    uint64_t instOff = 0, tTitle0 = 0;
    int firstWrite = 0;
    double sdBefore = -1.0;
    uint64_t waitHint0 = ts_now_ms(), winT0 = 0;
    uint32_t waitAcc = 0;
    int everPicked = 0, starvedFlag = 0;

    while(!wt->stop) {
        ts_wd_beat(wt->wd, 1);
        if(cur < 0) {
            if(!wanted_pending(wt)) {
                break; // tutto cio' che si voleva e' finito (installato o in errore)
            }

            int hint = wt->hintTitle >= 0 ? wt->hintTitle : wt->stateTitle;
            if(!everPicked && hint < 0 && ts_now_ms() - waitHint0 < 2000) {
                // la prima scelta aspetta di sapere cosa sta inviando il mittente (al massimo 2 s): aprire AM sul titolo sbagliato cancella subito la
                // versione vecchia e costringe a parcheggiare quello che arriva
                svcSleepThread(20ULL * 1000000ULL);
                continue;
            }

            int p = store_pick_active(S, hint);
            if(p < 0) {
                svcSleepThread(20ULL * 1000000ULL);
                continue;
            }

            // si attiva prima il titolo (la finestra comincia a raccogliere i suoi blocchi), poi si apre l'installazione in AM
            everPicked = 1;
            store_activate(S, p);
            winT0 = 0;
            waitAcc = 0;
            starvedFlag = 0;
            wt->curBytes = 0; // (the count of the title before must not show on this one while AM prepares it)
            wt->curTitle = p;
            uint64_t b0 = ts_now_ms();
            tTitle0 = b0;
            sdBefore = sd_free_gib();
            ts_wd_beat(wt->wd, 2);
            ts_amgate_begin(&wt->gate, (uint32_t) ts_now_ms(), TS_AM_STUCK_MS);
            Result r = ts_am_begin(m->t[p].tid, wt->exists[p], &h);
            // con la coda di 150 ms: fra la fine di ts_am_begin e la raccolta del primo gruppo il thread scrive due righe di registro e legge lo spazio libero
            // della SD, e il parcheggio non deve infilarsi proprio li' (la prima scrittura AM e' la piu' delicata)
            ts_amgate_end(&wt->gate, (uint32_t) ts_now_ms(), TS_AM_TAIL_MS);
            ts_log("titolo %d (%016llx, %.1f MiB): AM_StartCiaInstall -> 0x%08x in %llu ms (suggerito dal mittente: %d)", p, (unsigned long long) m->t[p].tid, (double) m->t[p].size / 1048576.0,
                   (unsigned int) r, (unsigned long long) (ts_now_ms() - b0), hint);
            if(R_FAILED(r)) {
                wt->result[p] = TS_RES_ERROR;
                wt->err[p] = (uint32_t) r;
                store_finish(S, p, 0);
                wt->curTitle = -1;
                continue;
            }

            cur = p;
            instOff = 0;
            wt->curBytes = 0;
            firstWrite = 1;
            ts_log("titolo %d: SD libera %.2f GiB prima dell'installazione (il titolo occupa %.2f GiB)", p, sdBefore, (double) m->t[p].size / 1073741824.0);
        }

        // "A secco" = l'installatore ha passato almeno 200 ms dei ultimi 300 ad ASPETTARE blocchi (AM non scrive): la SD e' libera e il titolo attivo puo'
        // parcheggiare anche oltre la finestra, invece di scartare cio' che passa mentre manca un blocco davanti. NON si misura con i blocchi consegnati ad AM:
        // un AM lento (scritture piccole, SD contesa) consegna poco ma non e' affamato, e parcheggiare in quel caso peggiora tutto (prova difficolta-2, 12.10).
        {
            uint64_t nowMs = ts_now_ms();
            if(winT0 == 0) {
                winT0 = nowMs;
            } else if(nowMs - winT0 >= 300) {
                int st2 = waitAcc >= 200;
                if(st2 != starvedFlag) {
                    starvedFlag = st2;
                    store_set_starved(S, st2);
                }

                winT0 = nowMs;
                waitAcc = 0;
            }
        }

        // Si raccolgono piu' corse (finestra e file) fino a TS_AM_AGG blocchi prima di scrivere: una scrittura AM costa ~12 ms fissi + ~0,45 ms per blocco, quindi con
        // corse da 8-20 KB (file frammentato) AM andava a 0,2-0,5 MiB/s e con 64+ blocchi a ~2 MiB/s (prova difficolta-2). Non si aspetta nessun dato: si
        // concatena solo cio' che e' gia' disponibile.
        size_t bytes = 0;
        uint64_t fileB = 0, winB = 0;
        int nb = 0, rdErr = 0;
        ts_wd_beat(wt->wd, 5);
        // Da qui AM "lavora": il parcheggio sta fermo da quando si comincia a raccogliere il gruppo (le letture dal file sono SD) fino a 150 ms dopo la fine
        // di ts_am_write. Se la raccolta non porta niente (installatore a secco) lo stato si spegne subito: la SD e' libera e il parcheggio puo' scrivere.
        // La clausola dei 20 s (operazione di AM impiccata) vale 60 s se nel gruppo c'e' la prima scrittura del titolo: AM prepara tutto lo spazio e per un titolo
        // da GiB dura normalmente fino a ~22 s (registri difficolta-3).
        ts_amgate_begin(&wt->gate, (uint32_t) ts_now_ms(), firstWrite ? TS_AM_STUCK_FIRST_MS : TS_AM_STUCK_MS);
        while(nb < TS_AM_AGG) {
            // una raccolta fatta di molte letture dal file non e' un blocco del thread: il watchdog segnala solo una singola chiamata che non torna
            ts_wd_beat(wt->wd, 5);
            size_t b1 = 0;
            int s1 = ST_SRC_NONE;
            int n1 = store_next_run(S, stage + bytes, (uint32_t) (TS_AM_AGG - nb), &b1, &s1);
            if(n1 < 0) {
                rdErr = 1;
                break;
            }

            if(n1 == 0) {
                break;
            }

            bytes += b1;
            nb += n1;
            if(s1 == ST_SRC_FILE) {
                fileB += b1;
            } else {
                winB += b1; // finestra, o coda di parcheggio (ST_SRC_QUEUE: blocchi in RAM presi direttamente, senza passare dal file): contano come RAM
            }
        }

        if(nb == 0) {
            ts_amgate_end(&wt->gate, (uint32_t) ts_now_ms(), 0);
        }

        if(nb == 0 && rdErr) {
            ts_log("titolo %d: lettura del file di parcheggio fallita: l'installazione si annulla", cur);
            ts_am_cancel(h);
            scrub_failed(wt, m->t[cur].tid);
            wt->result[cur] = TS_RES_ERROR;
            wt->err[cur] = 0xC8604C02;
            store_finish(S, cur, 0);
            cur = -1;
            wt->curTitle = -1;
            continue;
        }

        if(nb == 0) {
            // installatore a secco: i blocchi che gli servono non sono ancora arrivati (la finestra li aspetta, o stanno ancora venendo dal mittente)
            uint64_t sl0 = ts_now_ms();
            svcSleepThread(1000000);
            uint32_t slept = (uint32_t) (ts_now_ms() - sl0);
            wt->waitMs += slept;
            waitAcc += slept;
        } else {
            uint64_t w0 = ts_now_ms();
            ts_wd_beat(wt->wd, 3);
            Result r = ts_am_write(h, instOff, stage, (u32) bytes); // la prima scrittura di un titolo e' quella in cui AM prepara tutto: il parcheggio e' fermo dalla raccolta
            ts_amgate_end(&wt->gate, (uint32_t) ts_now_ms(), TS_AM_TAIL_MS);
            uint32_t wms = (uint32_t) (ts_now_ms() - w0);
            if(firstWrite) {
                // la prima scrittura di un titolo grosso e' quella in cui AM prepara lo spazio: e' qui che si sono visti i blocchi di 20 s
                firstWrite = 0;
                ts_log("titolo %d: prima scrittura AM %u ms (%d blocchi, offset %llu)", cur, (unsigned int) wms, nb, (unsigned long long) instOff);
            }

            wt->amLastMs = wms;
            wt->amTotalMs += wms;
            wt->amWrites++;
            wt->amHist[wms < 20 ? 0 : (wms < 50 ? 1 : (wms < 100 ? 2 : (wms < 500 ? 3 : 4)))]++;
            wt->amSizeHist[nb < 8 ? 0 : (nb < 16 ? 1 : (nb < 32 ? 2 : (nb < 64 ? 3 : 4)))]++;
            if(R_SUCCEEDED(r)) {
                wt->amBytes += bytes;
                store_mark_allocated(S, cur); // AM prepara tutto lo spazio del titolo alla prima scrittura: da qui e' gia' fuori dallo spazio libero letto
            }

            if(wms > wt->amMaxMs) {
                wt->amMaxMs = wms;
            }

            if(wms > 1500) {
                ts_log("scrittura AM lenta: %u ms (titolo %d, offset %llu, %d blocchi)", (unsigned int) wms, cur, (unsigned long long) instOff, nb);
            }

            if(R_FAILED(r)) {
                ts_log("scrittura AM fallita 0x%08x (titolo %d, offset %llu)", (unsigned int) r, cur, (unsigned long long) instOff);
                ts_am_cancel(h);
                scrub_failed(wt, m->t[cur].tid);
                wt->result[cur] = TS_RES_ERROR;
                wt->err[cur] = (uint32_t) r;
                store_finish(S, cur, 0);
                cur = -1;
                wt->curTitle = -1;
                continue;
            }

            instOff += bytes;
            wt->curBytes = instOff;
            wt->bytes += bytes;
            wt->fileBytes += fileB;
            wt->winBytes += winB;
            if(rdErr) {
                // le corse gia' consegnate sono state scritte; il file non si legge piu': l'installazione non puo' continuare
                ts_log("titolo %d: lettura del file di parcheggio fallita dopo %d blocchi: l'installazione si annulla", cur, nb);
                ts_am_cancel(h);
                scrub_failed(wt, m->t[cur].tid);
                wt->result[cur] = TS_RES_ERROR;
                wt->err[cur] = 0xC8604C02;
                store_finish(S, cur, 0);
                cur = -1;
                wt->curTitle = -1;
                continue;
            }
        }

        if(store_active_complete(S)) {
            uint64_t f0 = ts_now_ms();
            ts_wd_beat(wt->wd, 4);
            ts_amgate_begin(&wt->gate, (uint32_t) ts_now_ms(), TS_AM_STUCK_MS);
            Result r = ts_am_finish(h, &m->t[cur]);
            ts_amgate_end(&wt->gate, (uint32_t) ts_now_ms(), TS_AM_TAIL_MS); // come dopo begin: coprono le righe di registro che seguono (non la remove() del file in store_finish)
            ts_log("titolo %d: AM_FinishCiaInstall -> 0x%08x in %llu ms", cur, (unsigned int) r, (unsigned long long) (ts_now_ms() - f0));
            {
                double sdAfter = sd_free_gib();
                ts_log("titolo %d: SD libera %.2f GiB dopo l'installazione (calo %.2f GiB)", cur, sdAfter, sdBefore >= 0 && sdAfter >= 0 ? sdBefore - sdAfter : -1.0);
                double tsec = (double) (ts_now_ms() - tTitle0) / 1000.0;
                ts_log("titolo %d: ricevuto e installato in %.1f s, %.2f MiB/s medi (%.1f MiB)", cur, tsec, tsec > 0 ? (double) m->t[cur].size / 1048576.0 / tsec : 0.0, (double) m->t[cur].size / 1048576.0);
            }

            if(R_FAILED(r)) {
                // AM refused the end of the install: what it kept is taken away. A failure that is not AM's (the seed, added after AM finished) leaves the title as it was
                // (it is installed, and reported as an error), as before.
                if(R_MODULE(r) == RM_AM) {
                    scrub_failed(wt, m->t[cur].tid);
                }

                wt->result[cur] = TS_RES_ERROR;
                wt->err[cur] = (uint32_t) r;
            } else {
                wt->result[cur] = TS_RES_OK;
            }

            store_finish(S, cur, R_SUCCEEDED(r));
            cur = -1;
            wt->curTitle = -1;
        }
    }

    if(cur >= 0) {
        // fermato (dall'utente, dal sistema o perche' il mittente e' sparito) a meta' di un titolo: l'installazione aperta si annulla e cio' che AM ne ha gia' scritto
        // (il titolo, il ticket, l'importazione in corso) si toglie come dopo un errore, altrimenti sulla SD resta un titolo a meta' che il sistema elenca e nessuno usa
        ts_log("titolo %d: ricezione fermata a %llu byte: l'installazione si annulla e cio' che resta si toglie", cur, (unsigned long long) instOff);
        ts_am_cancel(h);
        scrub_failed(wt, m->t[cur].tid);
        wt->result[cur] = TS_RES_ERROR;
        wt->err[cur] = 0xC8604C03; // (fermata prima della fine: la pagina del risultato lo dice con queste parole)
        store_finish(S, cur, 0);
        wt->curTitle = -1;
    }

    free(stage);
    wt->finished = 1;
}

// ---------------------------------------------------------------- parcheggio (thread)

typedef struct {
    ts_store* store;
    writer_t* wt;
    uint32_t parkSlots;
    volatile int stop;
    volatile uint32_t freeMiB;      // spazio libero sulla SD (MiB), 0xFFFFFFFF finche' non e' stato letto
    volatile uint32_t parkErrors;
    volatile uint64_t tkBusy;
    // strumentazione (solo registro, scritti dal solo thread di parcheggio): giri in cui il blocco lo ha tenuto fermo (uno ogni ~5 ms di fermo) e passi fatti comunque
    // mentre il blocco era acceso, per valvola (coda al 75%) o per clausola (operazione di AM oltre 20 s, 60 s la prima scrittura)
    volatile uint32_t nHold, nValve, nClause;
    int wd;
} parker_t;

static void parker_fn(void* arg) {
    parker_t* pk = (parker_t*) arg;
    ts_log("parcheggio: thread avviato sul core %d", (int) svcGetProcessorID());
    uint64_t lastSpace = 0;
    int first = 1;
    while(!pk->stop) {
        ts_wd_beat(pk->wd, 1);
        uint64_t now = ts_now_ms();

        // La SD e' una sola: mentre AM lavora (raccolta e scrittura di un gruppo, apertura, prima scrittura, chiusura, piu' 150 ms dopo) il parcheggio sta
        // fermo e i blocchi aspettano in RAM; scrive comunque se la coda e' quasi piena (75%), per non scartare, o se un'operazione di AM dura da piu' di 20 s
        // (60 s la prima scrittura di un titolo), misurati dal suo inizio; vedi ts_amgate_hold. Prova C1: con il parcheggio in parallelo la prima scrittura AM
        // di un titolo da 0,1 MiB e' durata fino a 10 s e AM e' sceso a 0,5 MiB/s.
        int gate = ts_amgate_check(&pk->wt->gate, (uint32_t) now, store_park_queued(pk->store), pk->parkSlots);
        int hold = gate == TS_GATE_HELD;

        // Lo spazio libero della SD (una richiesta al servizio FS) si rinnova ogni 3 s quando il parcheggio puo' lavorare, ma solo ogni 30 s mentre e' fermo per AM:
        // durante un'operazione di AM la richiesta aspetta dietro di lei (nei registri fino a 19 s durante la prima scrittura) ed e' una richiesta FS in piu' proprio
        // quando la SD non va contesa. Quando il parcheggio scrive il valore non ha mai piu' di 3 s.
        // Dopo la richiesta si rilegge l'orologio e si rivaluta il blocco: la chiamata puo' essere durata a lungo.
        if(first || now - lastSpace >= (hold ? 30000u : 3000u)) {
            first = 0;
            lastSpace = now;
            ts_wd_beat(pk->wd, 2);
            uint64_t fb = sd_free_bytes();
            if(fb > 0) { // lettura fallita (0): si tengono i valori di prima
                store_update_space(pk->store, fb);
                pk->freeMiB = (uint32_t) (fb >> 20);
            }

            now = ts_now_ms();
            gate = ts_amgate_check(&pk->wt->gate, (uint32_t) now, store_park_queued(pk->store), pk->parkSlots);
            hold = gate == TS_GATE_HELD;
        }

        if(hold) {
            pk->nHold++;
            svcSleepThread(5000000);
            continue;
        }

        // Quando e cosa scrivere lo decide lo store (store_park_step): dal blocco piu' vecchio della coda, dopo circa 3 s, in tratti lunghi e in ordine. Qui non
        // si aspettano piu' "32 blocchi o 150 ms": quel raggruppamento e il riordino "AM ferma => svuota" frammentavano il file.
        int title = -1;
        ts_wd_beat(pk->wd, 3);
        uint64_t tk0 = svcGetSystemTick();
        int r = store_park_step(pk->store, &title);
        pk->tkBusy += svcGetSystemTick() - tk0;
        if(r != 0 && gate == TS_GATE_VALVE) {
            pk->nValve++; // ha scritto con il blocco acceso perche' la coda era al 75%
        } else if(r != 0 && gate == TS_GATE_CLAUSE) {
            pk->nClause++; // ha scritto con il blocco acceso perche' l'operazione di AM durava oltre il limite
        }
        if(r < 0) {
            pk->parkErrors++;
            ts_log("parcheggio: scrittura fallita per il titolo %d: non si accettano piu' blocchi di quel titolo finche' non torna lo spazio", title);
        } else if(r == 0) {
            svcSleepThread(5000000); // niente da scrivere ora: coda vuota, o la voce piu' vecchia e' ancora giovane
        }
    }
}

// ---------------------------------------------------------------- ricezione

// Differenza di tempi che non diventa mai enorme: il thread di rete puo' scrivere un istante piu' recente di `now` del thread principale.
static uint32_t age_ms(uint32_t now, uint32_t t) {
    return now >= t ? now - t : 0;
}

// Memoria per finestra e coda di parcheggio: TS_CACHE_PCT% della memoria libera, meta' e meta'. Se l'allocazione non riesce riprova con il 15% in meno.
// Ritorna 0 se riuscito (win e store creati).
static int alloc_buffers(ts_win* w, ts_store** store, const ts_manifest* m, const uint8_t* want, const ts_fileio* io, uint32_t* parkSlots) {
    uint32_t freeB = ts_mem_heap_free();
    // Minimo ridotto per gli Old 3DS (heap piu' piccolo): finestra 2048 + coda 1024 blocchi (4,4 MiB); sotto non si parte.
    const uint32_t minTotal = 3072;
    uint32_t total = ts_budget_blocks(freeB, TS_CACHE_PCT, TS_BLOCK + 8, TS_WIN_UNIT, minTotal, 2 * TS_WINDOW_MAX);
    ts_log("memoria: heap libero %.1f MiB, cache al %d%% -> %u blocchi da dividere fra finestra (70%%) e coda di parcheggio (30%%)", (double) freeB / 1048576.0, TS_CACHE_PCT, (unsigned int) total);
    if(total == 0) {
        total = minTotal; // poca memoria: si prova comunque il minimo
    }

    while(total >= minTotal) {
        // la finestra e' il tampone del titolo che si installa (assorbe gli stalli di AM e della SD): ha la parte maggiore; la coda serve ai titoli non attivi
        uint32_t wn = total * 70 / 100 / TS_WIN_UNIT * TS_WIN_UNIT;
        if(wn > TS_WINDOW_MAX) {
            wn = TS_WINDOW_MAX;
        }

        uint32_t pn = total - wn;
        if(win_init(w, wn) == 0) {
            *store = store_create(m, want, io, w, pn);
            if(*store != NULL) {
                *parkSlots = pn;
                return 0;
            }

            win_free(w);
            memset(w, 0, sizeof(*w));
        }

        total = (total * 85 / 100) / TS_WIN_UNIT * TS_WIN_UNIT;
    }

    return -1;
}

static uint32_t now_ms(uint64_t t0) {
    return (uint32_t) (ts_now_ms() - t0) + 1;
}

// Stato condiviso fra il thread di rete e quello principale (interfaccia e registro). Il thread di rete non tocca mai la SD
// ne' lo schermo: svuota la coda UDS, manda i riscontri, segue il collegamento e lo riprende se cade.
typedef struct {
    // ingressi
    udsBindContext* bind;
    ts_store* store;
    writer_t* wt;
    parker_t* pk;
    const ts_manifest* m;
    uint32_t session, total;
    uint16_t token;
    uint32_t winBlocks; // finestra del ricevente: si dichiara nel READY (flags = blocchi / TS_WIN_UNIT)
    uint8_t hostMac[6]; // indirizzo del mittente a cui siamo collegati: la riconnessione cerca solo quello (ce ne possono essere altri)
    uint16_t ident; // identificativo stabile di questa console (dall'ID0): il mittente riconosce un riavvio
    uint64_t t0;
    int wd;
    // controllo
    volatile int stop;
    // uscite
    volatile int endSeen;
    volatile int linkFailed;
    char failMsg[64];
    volatile int reconnecting;
    volatile int reconnFull;                             // durante la ricerca: la stanza c'e' (la scansione la trova) ma e' piena
    volatile int reconnNodes, reconnMax;                 // riceventi presenti / massimo, dal beacon trovato (0 = non noto)
    volatile uint32_t reconnAttempts;                    // tentativi di rientro (reconnects conta solo quelli riusciti)
    volatile int sessionLost;   // il mittente e' ripartito con una sessione nuova: questa non esiste piu'
    uint32_t otherSessN, otherSessAt;
    volatile int unbound; // 1 se la riconnessione ha sganciato il collegamento e non e' ancora tornata
    volatile uint32_t reconnStartMs;
    volatile int reconnects;
    volatile uint32_t lastData, lastUseful;
    volatile uint32_t maxFrameGapMs, nGap100, nGap500; // pausa piu' lunga fra due frame di dati e quante superano 100 e 500 ms (azzerate a ogni riga di registro)
    volatile uint32_t reconnTotalMs;                     // tempo totale passato a riconnettersi
    volatile uint64_t tkBusy;                            // tick in cui il thread di rete lavora (fuori dall'attesa di nuovi frame)
    volatile uint32_t maxBatch;                          // massimo di frame svuotati dalla coda UDS in un giro (vicino a ~130 = coda piena)
    volatile uint64_t tkAck;                             // tick spesi a comporre i riscontri
    volatile uint32_t maxAckMs;
    volatile int32_t clockOff;                           // orologio del mittente meno il mio (ms), dal riepilogo
    volatile int haveClockOff;
    volatile uint32_t ackSent, ackErr, pullErr;
    volatile uint64_t framesIn;
    volatile int nodes;
    volatile uint32_t linkStatus;
    volatile int rosterAgeDs;
    volatile uint32_t senderNext, senderKiBs, roomEtaSec;
    volatile int senderHears;
    volatile int nOthers;
    volatile uint32_t otherCursor[16];
    volatile uint8_t otherNode[16], otherState[16], otherDown[16]; // dal riepilogo del mittente (otherState 255 = non noto: mittente piu' vecchio)
    volatile int lastStateGen;
} net_t;

static ts_ack2 gAck2; // grande: non sullo stack

static void net_fn(void* arg) {
    net_t* n = (net_t*) arg;
    ts_store* S = n->store;
    writer_t* wt = n->wt;
    udsBindContext* bind = n->bind;
    ts_log("rete: thread avviato sul core %d", (int) svcGetProcessorID());
    uint8_t* rx = malloc(UDS_DATAFRAME_MAXSIZE);
    uint8_t* frame = malloc(UDS_DATAFRAME_MAXSIZE);
    static ts_roster ro;
    if(rx == NULL || frame == NULL) {
        snprintf(n->failMsg, sizeof(n->failMsg), "memoria insufficiente (rete)");
        n->linkFailed = 1;
        free(rx);
        free(frame);
        return;
    }

    uint32_t lastAck = 0, lastLink = 0, lostSince = 0, rosterBad = 0, lastForce = 0, lastFrame = 0, lastReady = 0;
    uint32_t startedAt = now_ms(n->t0);
    int forceRe = 0;
    n->lastData = n->lastUseful = startedAt;
    n->rosterAgeDs = -1;
    n->senderHears = -1;
    int lastTitle = 0; // titolo dell'ultimo blocco (per trovare il successivo senza cercare)

    while(!n->stop) {
        uint32_t now = now_ms(n->t0);
        uint64_t tkTop = svcGetSystemTick();
        uint32_t batch = 0;
        ts_wd_beat(n->wd, 1);

        // ---- ricezione dei frame
        int haveRoster = 0;
        for(;;) {
            size_t actual = 0;
            u16 src = 0;
            Result pr = udsPullPacket(bind, rx, UDS_DATAFRAME_MAXSIZE, &actual, &src);
            if(R_FAILED(pr)) {
                if(++n->pullErr <= 10) {
                    ts_log("udsPullPacket errore 0x%08x", (unsigned int) pr);
                }

                break;
            }

            if(actual == 0) {
                break;
            }

            batch++;
            uint8_t type, flags;
            uint32_t sess;
            uint16_t tk;
            (void) src;
            if(!ts_parse_hdr(rx, actual, &type, &flags, &sess, &tk)) {
                continue;
            }

            if(sess != n->session) {
                // il manifesto di un'altra sessione dalla stessa rete: il mittente e' ripartito (nuova stanza con lo stesso indirizzo)
                if(type == TS_T_MANIFEST) {
                    if(n->otherSessN == 0) {
                        n->otherSessAt = now;
                    }

                    if(++n->otherSessN >= 3 && now - n->otherSessAt >= 2000) {
                        n->sessionLost = 1;
                    }
                }

                continue;
            }

            if(type == TS_T_DATA) {
                uint32_t block;
                const uint8_t* payload;
                size_t len;
                n->framesIn++;
                if(ts_unpack_data(rx, actual, &block, &payload, &len)) {
                    // titolo del blocco (di solito e' lo stesso del precedente)
                    const ts_manifest* m = n->m;
                    int ti = lastTitle;
                    if(!(ti < m->count && block >= m->t[ti].firstBlock && block < m->t[ti].firstBlock + m->t[ti].blocks)) {
                        ti = -1;
                        for(int i = 0; i < m->count; i++) {
                            if(block >= m->t[i].firstBlock && block < m->t[i].firstBlock + m->t[i].blocks) {
                                ti = i;
                                break;
                            }
                        }
                    }

                    if(ti >= 0) {
                        lastTitle = ti;
                    }

                    // un frame di lunghezza diversa da quella attesa sposterebbe i dati: lo scarta store_offer. I titoli che non voglio li ignora subito.
                    int r = ST_OFFER_IGNORE;
                    if(ti >= 0 && ts_bit(wt->want, ti)) {
                        wt->hintTitle = ti;
                        r = store_offer(S, block, payload, len);
                    }

                    if(lastFrame != 0) {
                        uint32_t gap = now - lastFrame;
                        if(gap > n->maxFrameGapMs) {
                            n->maxFrameGapMs = gap;
                        }

                        if(gap > 100) {
                            n->nGap100++;
                        }

                        if(gap > 500) {
                            n->nGap500++;
                        }
                    }

                    lastFrame = now;
                    n->lastData = now;
                    if(r == ST_OFFER_WIN || r == ST_OFFER_PARK) {
                        n->lastUseful = now; // solo i blocchi che servono: quelli doppi o scartati non contano
                    }
                }
            } else if(type == TS_T_STATE) {
                uint8_t title, nr;
                uint16_t gen;
                if(ts_unpack_state(rx, actual, &title, &gen, &nr)) {
                    wt->stateTitle = title == TS_NO_TITLE ? -1 : (int) title;
                    n->lastStateGen = gen;
                    store_set_idle(S, title == TS_NO_TITLE); // "fine mandata": senza titolo in corso nessun vicino arrivera' piu', il parcheggio scrive subito
                }
            } else if(type == TS_T_ROSTER) {
                // dopo una pausa se ne accodano diversi: conta solo l'ultimo, gli altri descrivono un momento ormai passato
                if(ts_unpack_roster(rx, actual, &ro)) {
                    haveRoster = 1;
                }
            } else if(type == TS_T_END) {
                n->endSeen = 1;
            }
        }

        ts_wd_beat(n->wd, 2);
        if(haveRoster) {
            int age = -1;
            n->nOthers = 0;
            for(int i = 0; i < ro.n; i++) {
                if(ro.token[i] == n->token) {
                    age = ro.ageDs[i];
                } else if(n->nOthers < 16) {
                    n->otherCursor[n->nOthers] = ro.cursor[i];
                    n->otherNode[n->nOthers] = ro.ext ? ro.node[i] : 0;
                    n->otherState[n->nOthers] = ro.ext ? ro.state[i] : 255;
                    n->otherDown[n->nOthers] = ro.ext ? ro.down[i] : 0;
                    n->nOthers++;
                }
            }

            n->rosterAgeDs = age;
            n->clockOff = (int32_t) (ro.senderMs - (uint32_t) ts_log_ms());
            n->haveClockOff = 1;
            n->senderNext = ro.senderNext;
            n->roomEtaSec = ro.roomEtaSec;
            n->senderKiBs = ro.rateKiBs;
            n->senderHears = !(age < 0 || age > 50);

            // il mittente non mi sente (o non mi conosce) da piu' di 5 secondi mentre io ricevo: la strada di ritorno e' morta
            if(age < 0 || age > 50) {
                rosterBad++;
            } else {
                rosterBad = 0;
            }

            if(rosterBad >= 3 && !wt->finished && now - startedAt > 8000 && now - lastForce > 25000) {
                ts_log("il mittente non mi sente (eta' %d ds): riconnessione forzata", age);
                forceRe = 1;
                lastForce = now;
                rosterBad = 0;
            }
        }

        // ---- ACK2: un solo riscontro, unicast al mittente: riepilogo per titolo e blocchi mancanti del titolo che sta arrivando
        if(now - lastAck >= 120) {
            lastAck = now;
            uint64_t tkA = svcGetSystemTick();
            ts_ack2* a = &gAck2;
            ts_sum sum[TS_ACK2_MAX_SUM];
            int ns = store_summary(S, sum, TS_ACK2_MAX_SUM);
            a->nSum = ns;
            for(int i = 0; i < ns; i++) {
                a->sum[i] = sum[i];
            }

            // titolo di cui si mandano i buchi: l'ultimo che e' arrivato, se lo voglio ancora; poi quello annunciato dal mittente; poi il primo non finito
            int focus = -1;
            int cand[2] = {wt->hintTitle, wt->stateTitle};
            for(int c2 = 0; c2 < 2 && focus < 0; c2++) {
                for(int i = 0; i < ns; i++) {
                    if(cand[c2] >= 0 && sum[i].title == cand[c2]) {
                        focus = cand[c2];
                        break;
                    }
                }
            }

            if(focus < 0 && ns > 0) {
                focus = sum[0].title;
            }

            a->focus = focus < 0 ? TS_NO_TITLE : (uint8_t) focus;
            a->nRanges = focus >= 0 ? store_missing(S, focus, a->r, TS_ACK2_MAX_RANGES) : 0;
            uint32_t fm = n->pk->freeMiB;
            a->freeMiB = fm;
            a->installed = (uint32_t) (rd64(&wt->bytes) / TS_BLOCK);
            uint8_t ackFlags = 0;
            if(wt->finished) {
                ackFlags = TS_F_DONE;
                for(int i = 0; i < n->m->count; i++) {
                    if(wt->result[i] == TS_RES_ERROR) {
                        ackFlags |= TS_F_ERR; // (the sender shows "finished with errors")
                    }
                }
            }

            size_t len = ts_pack_ack2(frame, UDS_DATAFRAME_MAXSIZE, n->session, n->token, ackFlags, a);
            uint32_t ams = (uint32_t) ((svcGetSystemTick() - tkA) * 1000ULL / SYSCLOCK_ARM11);
            n->tkAck += svcGetSystemTick() - tkA;
            if(ams > n->maxAckMs) {
                n->maxAckMs = ams;
            }

            if(len > 0) {
                Result r1 = udsSendTo(UDS_HOST_NETWORKNODEID, TS_DATA_CHANNEL, UDS_SENDFLAG_Default, frame, len);
                n->ackSent++;
                if(R_FAILED(r1)) {
                    n->ackErr++;
                    if(n->ackErr <= 20) {
                        ts_log("invio riscontro: 0x%08x", (unsigned int) r1);
                    }
                }
            }
        }

        // ---- READY: il mittente puo' non conoscermi (e' ripartito, o ha perso il primo): lo ripeto ogni secondo, porta cio' che voglio e lo spazio libero
        if(now - lastReady >= 1000) {
            lastReady = now;
            size_t rn = ts_pack_ready(frame, UDS_DATAFRAME_MAXSIZE, (uint8_t) (n->winBlocks / TS_WIN_UNIT), n->session, n->token, n->m->count, wt->want, n->pk->freeMiB);
            if(rn > 0) {
                ts_set_hdr_ident(frame, n->ident);
                udsSendTo(UDS_HOST_NETWORKNODEID, TS_DATA_CHANNEL, UDS_SENDFLAG_Default, frame, rn);
            }
        }

        // ---- collegamento perso: si rientra nella rete e si riprende da dove si era
        ts_wd_beat(n->wd, 3);
        if(!wt->finished && now - lastLink >= 50) {
            lastLink = now;
            udsConnectionStatus cs;
            int okc = R_SUCCEEDED(udsGetConnectionStatus(&cs));
            n->nodes = okc ? (int) cs.total_nodes : -1;
            n->linkStatus = okc ? (uint32_t) cs.status : 0;
            int linkOk = okc && cs.total_nodes >= 2;
            if(linkOk) {
                lostSince = 0;
            } else if(lostSince == 0) {
                lostSince = now;
                ts_log("collegamento perso (installati %llu blocchi, ultimo dato %u ms fa)", (unsigned long long) (wt->bytes / TS_BLOCK), (unsigned int) (now - n->lastData));
            }

            if((lostSince != 0 && now - lostSince > 1500) || forceRe) {
                forceRe = 0;
                n->reconnAttempts++;
                n->reconnStartMs = lostSince != 0 ? lostSince : now; // the seconds that are shown count from the real drop, not from when the search began
                n->reconnecting = 1;
                n->reconnFull = 0;
                int back = 0;
                uint64_t tr = ts_now_ms();
                udsDisconnectNetwork();
                udsUnbind(bind);
                n->unbound = 1; // il contesto non e' piu' valido: alla chiusura non si sgancia una seconda volta (chiuderebbe un handle gia' chiuso)
                ts_log("riconnessione n.%u", (unsigned int) n->reconnAttempts);
                while(!back && !n->stop && ts_now_ms() - tr < TS_RECONNECT_MAX_MS) {
                    ts_wd_beat(n->wd, 20);
                    udsNetworkScanInfo* nets = NULL;
                    size_t total = 0;
                    if(receiver_scan_host(&nets, &total, n->hostMac) == 0 && total > 0) {
                        // the room is there: its beacon says how many are in it
                        const int inRoom = nets[0].network.total_nodes > 0 ? (int) nets[0].network.total_nodes - 1 : 0;
                        const int maxRoom = nets[0].network.max_nodes > 0 ? (int) nets[0].network.max_nodes - 1 : 0;
                        n->reconnNodes = inRoom;
                        n->reconnMax = maxRoom;
                        n->reconnFull = maxRoom > 0 && inRoom >= maxRoom; // (said at once: the connect that follows may take seconds)
                        Result cr = udsConnectNetwork(&nets[0].network, TS_PASSPHRASE, sizeof(TS_PASSPHRASE), bind, UDS_BROADCAST_NETWORKNODEID, UDSCONTYPE_Client, TS_DATA_CHANNEL,
                                                      TS_CLIENT_RECV_BUF);
                        back = R_SUCCEEDED(cr);
                        if(back) {
                            n->unbound = 0;
                        } else {
                            // found but not joined: with the room full that is the reason; otherwise it is the radio and the text stays "connection lost"
                            ts_log("riconnessione: udsConnectNetwork 0x%08x (stanza trovata: %d riceventi su %d%s)", (unsigned int) cr, inRoom, maxRoom, n->reconnFull ? ", piena" : "");
                        }
                    } else {
                        n->reconnFull = 0;
                    }

                    free(nets);
                    if(!back) {
                        svcSleepThread(50ULL * 1000000ULL);
                    }
                }

                n->reconnecting = 0;
                n->reconnFull = 0;
                if(!back) {
                    snprintf(n->failMsg, sizeof(n->failMsg), "%s", n->stop ? "annullato" : "mittente non ritrovato");
                    n->reconnTotalMs += (uint32_t) (ts_now_ms() - tr);
                    ts_log("riconnessione fallita dopo %llu s", (unsigned long long) ((ts_now_ms() - tr) / 1000));
                    n->linkFailed = 1;
                    break;
                }

                n->reconnects++; // (only the ones that worked)
                ts_log("riconnesso dopo %llu ms (tentativo %u)", (unsigned long long) (ts_now_ms() - tr), (unsigned int) n->reconnAttempts);
                n->reconnTotalMs += (uint32_t) (ts_now_ms() - tr);
                lastFrame = 0;
                lostSince = 0;
                n->lastData = n->lastUseful = now_ms(n->t0);
                lastAck = 0;
                startedAt = now_ms(n->t0); // concede un periodo di tolleranza dopo la riconnessione
                rosterBad = 0;
                continue;
            }
        }

        n->tkBusy += svcGetSystemTick() - tkTop;
        if(batch > n->maxBatch) {
            n->maxBatch = batch;
        }

        svcWaitSynchronization(bind->event, 1000000);
    }

    free(rx);
    free(frame);
}

// Strumentazione (solo registro, nessuna decisione cambia): le grandezze per giudicare la politica di scrittura e il blocco del parcheggio. I contatori dello store sono
// cumulativi (store_stats); quelli del file di parcheggio e del blocco vengono dai thread di installazione e di parcheggio (32 bit, un solo scrittore ciascuno).
// msPerRd = ms medi per lettura dal file, blkPerRd = blocchi medi per lettura (le divisioni si fanno qui, una volta ogni 10 s).
static void log_misure(const ts_store_stats* ss, const parkio_t* pio, const parker_t* pk, int final) {
    uint32_t rd = pio->nReads;
    uint32_t ms = pio->readMsTotal, blk = pio->readBlocks;
    ts_log("misure: %sparkByAge=%llu parkByValve=%llu parkByIdle=%llu parkAgeMax=%u queueBlocks=%llu qscanCalls=%llu qscanElems=%llu qscanMax=%u fileRd=%u fileRdMs=%u fileRdMax=%u fileRdBlk=%u msPerRd=%.1f blkPerRd=%.1f gateHold=%u gateValve=%u gateClause=%u",
           final ? "finale " : "", (unsigned long long) ss->parkByAge, (unsigned long long) ss->parkByValve, (unsigned long long) ss->parkByIdle, (unsigned int) ss->parkAgeMax,
           (unsigned long long) ss->queueBlocks, (unsigned long long) ss->qscanCalls, (unsigned long long) ss->qscanElems, (unsigned int) ss->qscanMax, (unsigned int) rd, (unsigned int) ms,
           (unsigned int) pio->readMsMax, (unsigned int) blk, rd ? (double) ms / (double) rd : 0.0, rd ? (double) blk / (double) rd : 0.0, (unsigned int) pk->nHold, (unsigned int) pk->nValve,
           (unsigned int) pk->nClause);
}

int receiver_run(ts_ctx* c, const udsNetworkScanInfo* net, int (*ui)(void* arg, ts_recv_stats* st), void* uiArg) {
    static ts_recv_stats st;
    memset(&st, 0, sizeof(st));
    st.phase = TS_R_CONNECT;
    st.curTitle = -1;
    st.freeMiB = 0xFFFFFFFFu;
    st.senderHears = -1;
    st.etaSec = -1;
    st.senderEtaSec = -1;
    snprintf(st.msg, sizeof(st.msg), "mi collego...");
    ui(uiArg, &st);
    ts_log_open("ricevente");
    ts_log_corrections(-1); // quale codice produce questo registro
    catalog_log_summary();
    park_cleanup();

    udsBindContext bind;
    Result r = -1;
    int connected = 0;
    for(int i = 0; i < 10 && !connected; i++) {
        r = udsConnectNetwork(&net->network, TS_PASSPHRASE, sizeof(TS_PASSPHRASE), &bind, UDS_BROADCAST_NETWORKNODEID, UDSCONTYPE_Client, TS_DATA_CHANNEL, TS_CLIENT_RECV_BUF);
        connected = R_SUCCEEDED(r);
    }

    if(!connected) {
        st.phase = TS_R_ERROR;
        snprintf(st.msg, sizeof(st.msg), "collegamento fallito (0x%08" PRIx32 ")", (uint32_t) r);
        ui(uiArg, &st);
        ts_log("collegamento fallito (0x%08x)", (unsigned int) r);
        ts_log_close();
        return -1;
    }

    st.channel = (int) net->network.channel; // (the radio channel of the room: the app shows it)
    ts_power_begin_transfer();
    uint8_t* rx = malloc(UDS_DATAFRAME_MAXSIZE);
    uint8_t* frame = malloc(UDS_DATAFRAME_MAXSIZE);
    ts_win win;
    memset(&win, 0, sizeof(win));
    ts_store* store = NULL;
    static parkio_t pio;
    memset(&pio, 0, sizeof(pio));
    static writer_t wt;
    memset(&wt, 0, sizeof(wt));
    ts_amgate_init(&wt.gate, (uint32_t) ts_now_ms());
    wt.wd = -1;
    wt.curTitle = wt.hintTitle = wt.stateTitle = -1;
    static parker_t pk;
    memset(&pk, 0, sizeof(pk));
    pk.wd = -1;
    pk.freeMiB = 0xFFFFFFFFu;
    static net_t nt;
    memset(&nt, 0, sizeof(nt));
    nt.wd = -1;
    nt.ident = ts_ident_from(c->devId);
    Thread th = NULL, thNet = NULL, thPark = NULL;
    int result = -1;
    int started = 0, decided = 0;
    int wdUi = ts_wd_add("interfaccia");
    uint32_t session = 0;
    uint64_t t0 = ts_now_ms();
    uint16_t token = (uint16_t) ((osGetTime() ^ (svcGetSystemTick() >> 4)) & 0xFFFF);
    if(token == 0) {
        token = 0x5A5A;
    }

    uint32_t otherSessN = 0, otherSessAt = 0;
    uint32_t lastUi = 0, doneAt = 0, lastLog = 0, pullErrPre = 0, lastDetail = 0, startedAtMs = 0;
    uint64_t lastFramesIn = 0, lastInstBytes = 0, lastNetBusy = 0, lastAckTk = 0, lastParkBusy = 0;
    double ownRate = 0; // blocchi al secondo consegnati ad AM, media mobile
    uint64_t etaBytes = 0;
    uint32_t etaT = 0;
    uint32_t rateT = 0;
    uint64_t rateBytes = 0;
    uint32_t needTotal = 0, parkSlots = 0, heldBlocks = 0, lastHeld = 0, lastMisure = 0;

    if(rx == NULL || frame == NULL) {
        goto cleanup;
    }

    st.phase = TS_R_WAIT_MANIFEST;
    snprintf(st.msg, sizeof(st.msg), "attendo l'elenco dei titoli...");

    // ============ prima dell'avvio: manifesto e scelta dei titoli (nel thread principale). Alla conferma si parte subito: non c'e' un giro da aspettare.
    for(;;) {
        uint32_t now = now_ms(t0);
        ts_wd_beat(wdUi, 10);
        if(!ts_power_poll()) {
            snprintf(st.msg, sizeof(st.msg), "il sistema chiede di chiudere");
            goto finish;
        }

        for(;;) {
            size_t actual = 0;
            u16 src = 0;
            Result pr = udsPullPacket(&bind, rx, UDS_DATAFRAME_MAXSIZE, &actual, &src);
            if(R_FAILED(pr)) {
                if(++pullErrPre <= 10) {
                    ts_log("udsPullPacket errore 0x%08x", (unsigned int) pr);
                }

                break;
            }

            if(actual == 0) {
                break;
            }

            uint8_t type, flags;
            uint32_t sess;
            uint16_t tk;
            (void) src;
            if(!ts_parse_hdr(rx, actual, &type, &flags, &sess, &tk)) {
                continue;
            }

            if(type == TS_T_MANIFEST) {
                if(st.haveManifest && sess != session) {
                    // un'altra sessione sulla stessa rete: il mittente e' ripartito mentre aspettavo
                    if(otherSessN == 0) {
                        otherSessAt = now;
                    }

                    if(++otherSessN >= 3 && now - otherSessAt >= 2000) {
                        snprintf(st.msg, sizeof(st.msg), "il mittente e' ripartito (sessione nuova): rientra nella stanza");
                        goto finish;
                    }
                } else if(!st.haveManifest && ts_unpack_manifest(rx, actual, &st.m) && ts_manifest_complete(&st.m)) {
                    st.haveManifest = 1;
                    session = st.m.session;
                    st.total = st.m.totalBlocks;
                    for(int i = 0; i < st.m.count; i++) {
                        st.exists[i] = (uint8_t) ts_title_installed(st.m.t[i].tid);
                        st.oldSize[i] = (st.exists[i] && ts_title_dest(st.m.t[i].tid) == MEDIATYPE_SD) ? ts_title_size(st.m.t[i].tid) : 0; // solo la SD: sulla NAND non si libera spazio SD
                        st.n3dsOnly[i] = (uint8_t) (!c->isNew && ((st.m.t[i].tid >> 28) & 0xF) == 2);
                        ts_set_bit(st.want, i, !st.exists[i] && !st.n3dsOnly[i]); // di base si prende cio' che manca (e che questa console puo' usare)
                    }

                    st.phase = TS_R_CHOOSE;
                    snprintf(st.msg, sizeof(st.msg), "scegli cosa ricevere");
                }
            }
        }

        // interfaccia (nella scelta si legge il tasto a ogni giro)
        if(now - lastUi >= (st.phase == TS_R_CHOOSE ? 0u : 100u)) {
            lastUi = now;
            int act = ui(uiArg, &st);
            if(act == TS_RUI_CANCEL) {
                snprintf(st.msg, sizeof(st.msg), "annullato");
                goto finish;
            }

            if(act == TS_RUI_CONFIRM && st.phase == TS_R_CHOOSE && !decided) {
                // buffer (finestra + coda di parcheggio) e archivio si creano ora: l'avvio non puo' fallire per memoria dopo
                pio.m = &st.m;
                ts_fileio io = {&pio, pio_open, pio_append, pio_read, pio_remove, pio_now};
                if(alloc_buffers(&win, &store, &st.m, st.want, &io, &parkSlots) != 0) {
                    snprintf(st.msg, sizeof(st.msg), "memoria insufficiente");
                    goto finish;
                }

                ts_log("finestra %u blocchi (%.1f MiB) + coda di parcheggio %u blocchi (%.1f MiB)", (unsigned int) win.W, (double) win.W * TS_BLOCK / 1048576.0, (unsigned int) parkSlots,
                       (double) parkSlots * TS_BLOCK / 1048576.0);
                decided = 1;

                for(int i = 0; i < st.m.count; i++) {
                    if(st.exists[i] && st.oldSize[i] > 0) {
                        store_set_reclaim(store, i, st.oldSize[i]); // sovrascrivere libera la versione vecchia: conta nel calcolo del parcheggio
                    }
                }

                wt.store = store;
                wt.m = &st.m;
                memcpy(wt.want, st.want, TS_BITMAP_BYTES);
                memcpy(wt.exists, st.exists, sizeof(wt.exists));
                for(int i = 0; i < st.m.count; i++) {
                    if(ts_bit(st.want, i)) {
                        wt.result[i] = TS_RES_PENDING;
                        needTotal += st.m.t[i].blocks;
                    } else {
                        wt.result[i] = TS_RES_SKIPPED;
                    }
                }

                s32 prio = 0x30;
                svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
                wt.wd = ts_wd_add("installazione");
                th = threadCreate(writer_fn, &wt, 64 * 1024, prio - 1, c->isNew ? 2 : -2, false);
                if(th == NULL) {
                    th = threadCreate(writer_fn, &wt, 64 * 1024, prio - 1, -2, false);
                }

                if(th == NULL) {
                    snprintf(st.msg, sizeof(st.msg), "thread di installazione non creato");
                    goto cleanup;
                }

                pk.store = store;
                pk.wt = &wt;
                pk.parkSlots = parkSlots;
                pk.wd = ts_wd_add("parcheggio");
                thPark = threadCreate(parker_fn, &pk, 64 * 1024, prio, c->isNew ? 2 : -2, false);
                if(thPark == NULL) {
                    thPark = threadCreate(parker_fn, &pk, 64 * 1024, prio, -2, false);
                }

                if(thPark == NULL) {
                    snprintf(st.msg, sizeof(st.msg), "thread di parcheggio non creato");
                    goto cleanup;
                }

                // thread di rete: priorita' piu' alta di quello principale e dello scrittore
                nt.bind = &bind;
                nt.store = store;
                nt.wt = &wt;
                nt.pk = &pk;
                nt.m = &st.m;
                nt.session = session;
                nt.total = st.total;
                nt.token = token;
                nt.winBlocks = win.W;
                memcpy(nt.hostMac, net->network.host_macaddress, 6);
                nt.t0 = t0;
                nt.wd = ts_wd_add("rete");
                thNet = threadCreate(net_fn, &nt, 64 * 1024, prio > 0x1A ? prio - 2 : prio - 1, -2, false);
                if(thNet == NULL) {
                    snprintf(st.msg, sizeof(st.msg), "thread di rete non creato");
                    goto cleanup;
                }

                started = 1;
                startedAtMs = now;
                ts_log("avvio ricezione: %d titoli, %u blocchi, voluti %u blocchi", st.m.count, (unsigned int) st.total, (unsigned int) needTotal);
                for(int i = 0; i < st.m.count; i++) {
                    ts_log("  titolo %d: %016llx %s %.1f MiB, blocchi %u + %u | voluto %s, gia' installato %s", i, (unsigned long long) st.m.t[i].tid, st.m.t[i].name, (double) st.m.t[i].size / 1048576.0,
                           (unsigned int) st.m.t[i].firstBlock, (unsigned int) st.m.t[i].blocks, ts_bit(st.want, i) ? "si'" : "no", st.exists[i] ? "si'" : "no");
                }

                ts_power_log_system("inizio ricezione");
                rateT = now;
                st.phase = TS_R_RECEIVE;
                snprintf(st.msg, sizeof(st.msg), "ricezione in corso");
            }
        }

        if(started) {
            break;
        }

        svcWaitSynchronization(bind.event, 1000000);
    }

    // ============ dopo l'avvio: la rete, il parcheggio e l'installazione stanno nei loro thread; qui restano interfaccia, registro e fine
    for(;;) {
        uint32_t now = now_ms(t0);
        ts_wd_beat(wdUi, 20);
        if(!ts_power_poll()) {
            snprintf(st.msg, sizeof(st.msg), "il sistema chiede di chiudere");
            result = -1;
            break;
        }

        if(wt.finished) {
            if(doneAt == 0) {
                doneAt = now;
            }

            if(nt.endSeen || now - doneAt > 4000) {
                result = 0;
                for(int i = 0; i < st.m.count; i++) {
                    if(wt.result[i] == TS_RES_ERROR) {
                        result = 1;
                    }
                }

                break;
            }
        }

        if(nt.linkFailed) {
            snprintf(st.msg, sizeof(st.msg), "%s", nt.failMsg);
            result = -1;
            break;
        }

        // ---- registro periodico
        if(now - lastLog >= 2000) {
            lastLog = now;
            ts_store_stats ss;
            store_stats(store, &ss);
            ts_log("stato: installato %llu/%u blocchi (titolo %d, suggerito %d, mittente su %d) | offerti %llu finestra %llu parcheggiati %llu doppi %llu scartati: coda piena %llu non accettati %llu oltre finestra %llu | file: blocchi scritti %llu letti %llu errori %llu | coda %u picco %u | ultimo dato %u ms fa utile %u ms fa | riscontri %u err %u letture KO %u | nodi %d stato 0x%x | roster eta' %d ds | AM ultimo %u max %u ms | SD libera %u MiB%s",
                   (unsigned long long) (wt.bytes / TS_BLOCK), (unsigned int) needTotal, (int) wt.curTitle, (int) wt.hintTitle, (int) wt.stateTitle, (unsigned long long) ss.offered,
                   (unsigned long long) ss.win, (unsigned long long) ss.parked, (unsigned long long) ss.dup, (unsigned long long) ss.dropFull, (unsigned long long) ss.dropNoAccept, (unsigned long long) ss.dropWin,
                   (unsigned long long) ss.fileBlocks, (unsigned long long) ss.fileReads, (unsigned long long) ss.parkErrors, (unsigned int) store_park_queued(store), (unsigned int) ss.parkPeak,
                   (unsigned int) age_ms(now, nt.lastData), (unsigned int) age_ms(now, nt.lastUseful), (unsigned int) nt.ackSent, (unsigned int) nt.ackErr, (unsigned int) nt.pullErr, nt.nodes,
                   (unsigned int) nt.linkStatus, nt.rosterAgeDs, (unsigned int) wt.amLastMs, (unsigned int) wt.amMaxMs, (unsigned int) pk.freeMiB, wt.finished ? " (installazione finita)" : "");
            {
                // riepilogo per titolo: come lo vede il mittente
                ts_sum sum[TS_ACK2_MAX_SUM];
                int ns = store_summary(store, sum, TS_ACK2_MAX_SUM);
                char line[240];
                int ll = 0;
                line[0] = 0;
                for(int i = 0; i < ns && ll < (int) sizeof(line) - 40; i++) {
                    ll += snprintf(line + ll, sizeof(line) - (size_t) ll, " t%d:%u/%u%s%s", sum[i].title, (unsigned int) sum[i].remain, (unsigned int) st.m.t[sum[i].title].blocks,
                                   (sum[i].flags & TS_SF_INSTALLING) ? "*" : "", (sum[i].flags & TS_SF_ACCEPT) ? "" : "!");
                }

                ts_log("    titoli (mancano/blocchi, * = in installazione, ! = non accetta):%s", ns ? line : " nessuno");
            }

            // dettagli: pause fra i frame, installatore, parcheggio
            double dtl = (double) (now - lastDetail) / 1000.0;
            if(dtl < 0.1) {
                dtl = 0.1;
            }

            ts_log("    dettagli: frame ricevuti/s %.0f | pausa max fra frame %u ms (>100 ms: %u, >500 ms: %u) | AM %u scritture %u ms totali, a secco %u ms | consegnato ad AM %.2f MiB/s (da file %.1f MiB, da finestra %.1f MiB) | riconnessioni %d (%u ms) | wifi %u/3 | orologio mittente - mio %s%d ms",
                   (double) (nt.framesIn - lastFramesIn) / dtl, (unsigned int) nt.maxFrameGapMs, (unsigned int) nt.nGap100, (unsigned int) nt.nGap500, (unsigned int) wt.amWrites,
                   (unsigned int) wt.amTotalMs, (unsigned int) wt.waitMs, (double) (wt.bytes - lastInstBytes) / 1048576.0 / dtl, (double) wt.fileBytes / 1048576.0, (double) wt.winBytes / 1048576.0,
                   nt.reconnects, (unsigned int) nt.reconnTotalMs, (unsigned int) osGetWifiStrength(), nt.haveClockOff ? "" : "n.d. ", nt.haveClockOff ? (int) nt.clockOff : 0);
            ts_log("    rete e AM: AM %.2f MiB/s mentre scrive (media %.1f ms/scrittura), durata scritture <20 ms: %u, 20-50: %u, 50-100: %u, 100-500: %u, >=500: %u | thread di rete occupato %.0f%% (riscontri %.1f%%, max %u ms), frame max per giro %u (coda UDS ~130) | parcheggio occupato %.0f%%, scritture file %u (max %u ms), letture file %u (max %u ms) | blocchi per scrittura AM <8: %u, 8-15: %u, 16-31: %u, 32-63: %u, >=64: %u",
                   wt.amTotalMs ? (double) wt.amBytes / 1048576.0 / ((double) wt.amTotalMs / 1000.0) : 0.0, wt.amWrites ? (double) wt.amTotalMs / (double) wt.amWrites : 0.0, (unsigned int) wt.amHist[0],
                   (unsigned int) wt.amHist[1], (unsigned int) wt.amHist[2], (unsigned int) wt.amHist[3], (unsigned int) wt.amHist[4],
                   100.0 * (double) (nt.tkBusy - lastNetBusy) / ((double) SYSCLOCK_ARM11 * dtl), 100.0 * (double) (nt.tkAck - lastAckTk) / ((double) SYSCLOCK_ARM11 * dtl), (unsigned int) nt.maxAckMs,
                   (unsigned int) nt.maxBatch, 100.0 * (double) (pk.tkBusy - lastParkBusy) / ((double) SYSCLOCK_ARM11 * dtl), (unsigned int) pio.nWrites, (unsigned int) pio.maxWriteMs,
                   (unsigned int) pio.nReads, (unsigned int) pio.maxReadMs, (unsigned int) wt.amSizeHist[0], (unsigned int) wt.amSizeHist[1], (unsigned int) wt.amSizeHist[2],
                   (unsigned int) wt.amSizeHist[3], (unsigned int) wt.amSizeHist[4]);
            if(lastMisure == 0 || now - lastMisure >= 9000) {
                lastMisure = now;
                log_misure(&ss, &pio, &pk, 0);
            }

            lastNetBusy = nt.tkBusy;
            lastAckTk = nt.tkAck;
            lastParkBusy = pk.tkBusy;
            nt.maxBatch = 0;
            nt.maxAckMs = 0;
            pio.maxWriteMs = 0;
            pio.maxReadMs = 0;
            lastDetail = now;
            lastFramesIn = nt.framesIn;
            lastInstBytes = wt.bytes;
            nt.maxFrameGapMs = 0;
            nt.nGap100 = 0;
            nt.nGap500 = 0;
        }

        // ---- interfaccia
        if(now - lastUi >= 150) {
            lastUi = now;
            if(etaT == 0) {
                etaT = now;
                etaBytes = wt.bytes;
            } else if(now - etaT >= 1000) {
                double inst = (double) (wt.bytes - etaBytes) / TS_BLOCK / ((double) (now - etaT) / 1000.0);
                ownRate = ownRate == 0 ? inst : 0.9 * ownRate + 0.1 * inst;
                etaT = now;
                etaBytes = wt.bytes;
            }

            uint32_t instBlocks = (uint32_t) (wt.bytes / TS_BLOCK);
            uint32_t remMine = needTotal > instBlocks ? needTotal - instBlocks : 0;
            st.etaSec = (wt.finished || remMine == 0) ? 0 : (ownRate > 20.0 ? (double) remMine / ownRate : -1.0);
            st.senderEtaSec = nt.roomEtaSec == 0xFFFF ? -1.0 : (double) nt.roomEtaSec; // tempo stimato dal mittente per l'ultimo ricevente della stanza
            st.senderNext = nt.senderNext; // completamento della stanza (%)
            st.senderKiBs = nt.senderKiBs;
            st.senderHears = nt.senderHears;
            st.nOthers = nt.nOthers;
            for(int i = 0; i < st.nOthers && i < 16; i++) {
                st.otherCursor[i] = nt.otherCursor[i];
                st.otherNode[i] = nt.otherNode[i];
                st.otherState[i] = nt.otherState[i];
                st.otherDown[i] = nt.otherDown[i];
            }

            ts_store_stats ss;
            store_stats(store, &ss);
            st.reconnects = nt.reconnects;
            st.dataAgeMs = age_ms(now, nt.lastData);
            st.needBlocks = needTotal;
            // scaricato = blocchi che ho (in finestra, in coda, su file o gia' installati) = voluto - cio' che manca ancora secondo il riepilogo (stesso numero che vede il mittente)
            if(now - lastHeld >= 1000 || lastHeld == 0) {
                lastHeld = now;
                ts_sum hs[TS_ACK2_MAX_SUM];
                int hn = store_summary(store, hs, TS_ACK2_MAX_SUM);
                uint32_t rem = 0;
                for(int i = 0; i < hn; i++) {
                    rem += hs[i].remain;
                }

                heldBlocks = rem >= needTotal ? 0 : needTotal - rem;
            }

            st.dlBlocks = heldBlocks > needTotal ? needTotal : heldBlocks;
            {
                int errs = 0;
                for(int i = 0; i < st.m.count; i++) {
                    errs += wt.result[i] == TS_RES_ERROR;
                }

                st.allDone = wt.finished && errs == 0;
            }

            st.instBlocks = instBlocks;
            st.cursor = instBlocks;
            st.freeMiB = pk.freeMiB;
            st.curInstBytes = wt.curBytes;
            st.reconnecting = nt.reconnecting;
            st.roomFull = nt.reconnecting && nt.reconnFull;
            st.roomNodes = nt.reconnNodes;
            st.roomMax = nt.reconnMax;
            st.recv = ss.win + ss.parked;
            st.dups = ss.dup;
            st.outOfWindow = ss.dropFull + ss.dropNoAccept;
            st.curTitle = wt.curTitle;
            st.amLastMs = wt.amLastMs;
            st.amMaxMs = wt.amMaxMs;
            for(int i = 0; i < st.m.count; i++) {
                st.result[i] = wt.result[i];
                st.err[i] = wt.err[i];
            }

            if(now - rateT >= 1000) {
                st.mibps = (double) (wt.bytes - rateBytes) / 1048576.0 / ((double) (now - rateT) / 1000.0);
                rateT = now;
                rateBytes = wt.bytes;
            }

            if(nt.reconnecting) {
                const unsigned int lostSecs = (unsigned int) ((now - nt.reconnStartMs) / 1000);
                if(nt.reconnFull) {
                    snprintf(st.msg, sizeof(st.msg), "stanza piena (%d/%d): riprovo (%u s) B = rinuncia", (int) nt.reconnNodes, (int) nt.reconnMax, lostSecs);
                } else {
                    snprintf(st.msg, sizeof(st.msg), "collegamento perso: cerco il mittente... (%u s) B = rinuncia", lostSecs);
                }
            } else if(!wt.finished && needTotal > 0 && st.dlBlocks >= needTotal) {
                snprintf(st.msg, sizeof(st.msg), "tutto scaricato: installo"); // (nothing is missing any more: the sender has nothing more to send to this console)
            } else if(!wt.finished && age_ms(now, nt.lastUseful) > 3000 && now - startedAtMs > 3000) {
                snprintf(st.msg, sizeof(st.msg), "in attesa: il mittente sta servendo altri");
            } else if(nt.reconnects > 0) {
                snprintf(st.msg, sizeof(st.msg), "ricezione in corso (riconnesso %d volte)", nt.reconnects);
            } else {
                st.msg[0] = 0;
            }

            if(nt.sessionLost && !wt.finished) {
                snprintf(st.msg, sizeof(st.msg), "il mittente e' ripartito (sessione nuova): rientra nella stanza");
                result = -1;
                break;
            }

            if(nt.endSeen && !wt.finished) {
                snprintf(st.msg, sizeof(st.msg), "il mittente ha chiuso la stanza");
                result = -1;
                break;
            }

            int act = ui(uiArg, &st);
            if(act == TS_RUI_CANCEL) {
                snprintf(st.msg, sizeof(st.msg), "annullato");
                break;
            }
        }

        svcSleepThread(10ULL * 1000000ULL);
    }

finish:
    st.phase = result == 0 ? TS_R_DONE : TS_R_ERROR;
    if(result == 0) {
        snprintf(st.msg, sizeof(st.msg), "installazione completata");
    } else if(result == 1) {
        snprintf(st.msg, sizeof(st.msg), "finito con errori");
    }

    if(started) {
        st.cursor = (uint32_t) (wt.bytes / TS_BLOCK);
        for(int i = 0; i < st.m.count; i++) {
            st.result[i] = wt.result[i];
            st.err[i] = wt.err[i];
        }
    }

    ui(uiArg, &st);

cleanup:
    // prima si fermano i thread, poi si chiude il registro: cosi' ci sono anche le ultime righe
    nt.stop = 1;
    if(thNet != NULL) {
        threadJoin(thNet, U64_MAX);
        threadFree(thNet);
    }

    if(started) {
        ts_store_stats ss;
        store_stats(store, &ss);
        double secs = (double) (now_ms(t0) - startedAtMs) / 1000.0;
        ts_log("riepilogo ricezione: durata %.1f s, installati %.1f MiB (%.2f MiB/s medi), blocchi offerti %llu in finestra %llu parcheggiati %llu doppi %llu scartati (coda piena %llu, non accettati %llu), su file %llu blocchi (%llu letti, %llu errori di scrittura), riconnessioni %d (%u ms in riconnessione), AM %u scritture %u ms, installatore a secco %u ms, consegnato da file %.1f MiB da finestra %.1f MiB",
               secs, (double) wt.bytes / 1048576.0, secs > 0 ? (double) wt.bytes / 1048576.0 / secs : 0.0, (unsigned long long) ss.offered, (unsigned long long) ss.win, (unsigned long long) ss.parked,
               (unsigned long long) ss.dup, (unsigned long long) ss.dropFull, (unsigned long long) ss.dropNoAccept, (unsigned long long) ss.fileBlocks, (unsigned long long) ss.fileReads,
               (unsigned long long) ss.parkErrors, nt.reconnects, (unsigned int) nt.reconnTotalMs, (unsigned int) wt.amWrites, (unsigned int) wt.amTotalMs, (unsigned int) wt.waitMs,
               (double) wt.fileBytes / 1048576.0, (double) wt.winBytes / 1048576.0);
        log_misure(&ss, &pio, &pk, 1);
    }

    ts_log("fine ricezione: risultato %d (%s), installati %llu di %u blocchi", result, st.msg[0] != 0 ? st.msg : "motivo non registrato", (unsigned long long) (wt.bytes / TS_BLOCK), (unsigned int) needTotal);
    if(th != NULL) {
        wt.stop = 1;
        threadJoin(th, U64_MAX);
        threadFree(th);
    }

    if(thPark != NULL) {
        pk.stop = 1;
        threadJoin(thPark, U64_MAX);
        threadFree(thPark);
    }

    // i file temporanei si chiudono e si cancellano (anche se la ricezione e' stata annullata)
    if(store != NULL) {
        for(int i = 0; i < st.m.count; i++) {
            if(pio.isOpen[i]) {
                pio_remove(&pio, i);
            }
        }

        store_destroy(store);
        store = NULL;
    }

    ts_wd_remove(nt.wd);
    ts_wd_remove(wt.wd);
    ts_wd_remove(pk.wd);
    ts_wd_remove(wdUi);
    ts_power_end_transfer();

    // Chiusura. Il registro resta aperto fino alla fine e ogni passo e' sorvegliato: se qualcosa si blocca, il registro dice dove.
    int wdEnd = ts_wd_add("chiusura");
    ts_wd_beat(wdEnd, 1);

    // uscita pulita: il mittente libera subito lo slot (se non arriva, lo libera dopo TS_S_LOST_MS). Solo se il mittente ci conosce
    // (abbiamo mandato almeno un READY): chi annulla durante la scelta non gli e' mai stato annunciato.
    if(frame != NULL && st.haveManifest && decided) {
        size_t bn = ts_pack_simple(frame, UDS_DATAFRAME_MAXSIZE, TS_T_BYE, 0, session, token);
        for(int i = 0; i < 3 && bn > 0; i++) {
            udsSendTo(UDS_HOST_NETWORKNODEID, TS_DATA_CHANNEL, UDS_SENDFLAG_Default, frame, bn);
            svcSleepThread(40ULL * 1000000ULL);
        }

        ts_log("uscita pulita: inviato BYE al mittente");
    }

    ts_wd_beat(wdEnd, 2);
    ts_log("chiusura: scollego dalla rete...");
    ts_log_flush();
    udsDisconnectNetwork();
    ts_wd_beat(wdEnd, 3);
    ts_log("chiusura: scollegato, rilascio il collegamento...");
    ts_log_flush();
    if(!nt.unbound) {
        udsUnbind(&bind);
    }

    ts_wd_beat(wdEnd, 4);
    ts_log("chiusura: rete rilasciata, torno al menu");
    ts_log_flush();
    ts_wd_remove(wdEnd);
    ts_log_close();

    if(win.data != NULL) {
        win_free(&win);
    }

    free(rx);
    free(frame);
    return result;
}
