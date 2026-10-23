#include "sender.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aes.h"
#include "buildcfg.h"
#include "catalog.h"
#include "ciastream.h"
#include "log.h"
#include "membudget.h"
#include "net.h"
#include "pace.h"
#include "runcache.h"
#include "sched.h"
#include "power.h"
#include "seed.h"

static int gPaceMode = 7; // predefinito: libero (senza passo). Con un ricevente adattivo e libero rendono uguale (2,0 MiB/s), con piu' riceventi l'adattivo scende per la perdita di UN ricevente (prova a 4 DS del 12.10: fino a 450 blocchi/s) mentre la perdita non dipende dal passo; si cambia con Y a stanza vuota

int sender_pace_mode(void) {
    return gPaceMode;
}

void sender_pace_next(void) {
    gPaceMode = (gPaceMode + 1) % TS_PACE_MODES;
}

// Blocchi che il lettore puo' leggere in anticipo rispetto al mittente: un quarto dell'anello, da 512 a 2048. Il resto
// dell'anello e' la storia dei blocchi gia' inviati, che serve per le ripetizioni senza rileggere dalla SD.
static inline uint32_t read_ahead(uint32_t ringN) {
    uint32_t ra = ringN / 4;
    return ra < 512 ? 512 : (ra > 2048 ? 2048 : ra);
}

#define TS_RUN_BLOCKS 16 // margine dell'anello in ring_slot (la lettura in corso del lettore); indipendente da TS_SND_RUN_BLOCKS, la lunghezza di una corsa della cache

// Cache delle ripetizioni lette dalla SD fuori dall'anello (common/runcache.[ch]): TS_SND_RUNS corse di TS_SND_RUN_BLOCKS blocchi consecutivi ciascuna,
// sostituzione LRU (la meno usata di recente). Prima erano 2 corse da 16 a turno: in recupero i flussi di blocchi consecutivi sono 3-4 o piu' (marcia, ritardatari,
// buchi del turno) e con 2 corse si rileggeva quasi sempre.
// Quante corse: TS_SND_RUNS, di base 16 (364 KiB), definito in buildcfg.h (un solo posto: lo dichiara anche il registro); make SND_RUNS=n (1-64) per una prova. 64 e' il
// massimo (1,42 MiB, l'1,6% dello heap di una New 3DS): non servono per i flussi sequenziali (bastano 8-16) ma per i blocchi persi di nuovo dai riceventi lontani, che tornano a
// distanza di secondi. In simulazione con 7 riceventi con perdite diverse e costanti l'ultimo finisce il 6-16% prima che con 16 corse e il 3-10% prima che con 32; con 3-4
// riceventi il tempo non cambia (+-1-2%) e con 64 i frame inviati salgono del 2% circa (un mittente piu' veloce manda piu' ripetizioni con gli ACK ancora in volo: a parita' di
// ritmo la differenza e' <= 0,6%). Se la memoria non basta alloc_runs dimezza fino a 2.
#ifndef TS_SND_RUN_BLOCKS
#define TS_SND_RUN_BLOCKS 16
#endif
#if TS_SND_RUNS < 1 || TS_SND_RUNS > TS_RC_MAX_SLOTS
#error "TS_SND_RUNS deve stare fra 1 e TS_RC_MAX_SLOTS (64)"
#endif

// Alloca l'anello: TS_CACHE_PCT% della memoria libera, fra TS_RING_MIN e TS_RING_MAX blocchi. Se l'allocazione non riesce
// riprova con il 15% in meno.
static uint8_t* alloc_ring(uint32_t* nOut) {
    uint32_t freeB = ts_mem_heap_free();
    uint32_t n = ts_budget_blocks(freeB, TS_CACHE_PCT, TS_BLOCK, 16, TS_RING_MIN, TS_RING_MAX);
    ts_log("memoria: heap libero %.1f MiB, cache al %d%% -> anello %u blocchi", (double) freeB / 1048576.0, TS_CACHE_PCT, (unsigned int) n);
    if(n == 0) {
        n = TS_RING_MIN;
    }

    while(n >= TS_RING_MIN) {
        uint8_t* p = malloc((size_t) n * TS_BLOCK);
        if(p != NULL) {
            *nOut = n;
            return p;
        }

        n = (n * 85 / 100) / 16 * 16;
    }

    return NULL;
}

// Memoria delle corse della cache delle ripetizioni (TS_SND_RUNS corse da TS_SND_RUN_BLOCKS blocchi), in un blocco solo dello heap: come l'anello e come
// le due corse di prima, perche' la riempie fread (FSFILE_Read) e poi la decifra la CPU; nessun DMA o GPU la tocca, quindi non serve la memoria lineare.
// Si alloca DOPO l'anello: l'anello prende il suo TS_CACHE_PCT% del libero come prima e le corse stanno nel resto (a 64 corse 1,4 MiB contro ~22 MiB
// che restano). Se non entra riprova con la meta' delle corse, fino a 2 (come prima).
// Riserva: oltre alle 2 corse di prima (93 KiB), una cache piu' grande si prende solo se dopo di lei restano TS_RUNS_RESERVE_B di heap liberi: sender_run alloca ancora
// lo stack del lettore (64 KiB), la prova AES (1 MiB, facoltativa) e, a regime, lo scratch e il TMD di cia_stream (meno di 0,3 MiB obbligatori). Con lo heap della New
// 3DS (20 MiB liberi dopo l'anello e le corse) non cambia nulla; con uno heap molto piu' piccolo la cache scende prima di toglierli alle allocazioni successive.
#define TS_RUNS_RESERVE_B (2u << 20)
static uint8_t* alloc_runs(uint32_t* nOut) {
    uint32_t n = TS_SND_RUNS;
    uint32_t freeB = ts_mem_heap_free();
    for(;;) {
        size_t sz = ts_rc_mem_size(n, TS_SND_RUN_BLOCKS);
        uint8_t* p = (n <= 2 || sz + TS_RUNS_RESERVE_B <= freeB) ? malloc(sz) : NULL;
        if(p != NULL) {
            *nOut = n;
            return p;
        }

        if(n <= 2) {
            return NULL;
        }

        n = n / 2 < 2 ? 2 : n / 2;
    }
}

// Tre flussi virtuali: uno per il controllo, uno per il lettore (blocchi nuovi), uno per le riparazioni.
static cia_stream gCheck, gFresh, gRepair;

int sender_check_title(ts_ctx* c, u64 tid, ts_title* t, char* err, size_t errSize) {
    err[0] = 0;
    if(!c->haveKeys) {
        snprintf(err, errSize, "chiavi non disponibili");
        return -1;
    }

    int rc = cia_stream_open(&gCheck, tid, c->ctrRoot, c->rk, c->certsDb, c->certsDbSize);
    if(rc != 0) {
        snprintf(err, errSize, "%s", gCheck.err[0] ? gCheck.err : "titolo non leggibile");
        return -2;
    }

    if(((gCheck.front.total + TS_BLOCK - 1) / TS_BLOCK) * (uint64_t) TS_BLOCK > 0xFFFFFFFFULL) {
        // il file temporaneo dei riceventi (e il file di AM) non supera i 4 GiB: limite di FAT32
        snprintf(err, errSize, "oltre 4 GiB (limite FAT32)");
        cia_stream_close(&gCheck);
        return -4;
    }

    memset(t, 0, sizeof(*t));
    t->tid = tid;
    t->size = gCheck.front.total;
    ctx_product_code(tid, t->code);

    // Il content con indice piu' basso porta l'intestazione NCCH: il flag 0x20 dice se serve il seed.
    int best = -1;
    for(int i = 0; i < gCheck.front.n; i++) {
        if(gCheck.front.c[i].size >= 0x200 && (best < 0 || gCheck.front.c[i].index < gCheck.front.c[best].index)) {
            best = i;
        }
    }

    if(best >= 0) {
        uint8_t h[0x200];
        if(cia_stream_read(&gCheck, gCheck.front.start[best], h, sizeof(h)) == 0 && memcmp(h + 0x100, "NCCH", 4) == 0 && (h[0x18F] & 0x20)) {
            size_t where = 0;
            if(c->seedDb == NULL || seed_find(c->seedDb, c->seedDbSize, tid, h + 0x114, t->seed, &where) != 0) {
                snprintf(err, errSize, "seed non trovato");
                cia_stream_close(&gCheck);
                return -3;
            }

            t->hasSeed = 1;
        }
    }

    cia_stream_close(&gCheck);
    return 0;
}

int sender_make_manifest(const ts_title* t, int n, ts_manifest* m) {
    memset(m, 0, sizeof(*m));
    if(n < 1 || n > TS_MAX_TITLES) {
        return -1;
    }

    memcpy(m->t, t, sizeof(ts_title) * (size_t) n);
    m->count = n;
    m->session = (uint32_t) (osGetTime() ^ (svcGetSystemTick() >> 3)) | 1;
    ts_manifest_finalize(m);
    return 0;
}

// ---------------------------------------------------------------- lettore (thread)

typedef struct {
    const ts_manifest* m;
    ts_ctx* c;
    cia_stream* s;
    int title;
    uint8_t* ring;
    uint32_t ringN;
    ts_range skip[TS_MAX_TITLES + 1];
    int nSkip;
    volatile uint32_t readPtr, sendPtr;
    volatile int stop, error;
    volatile int pause, paused;  // il mittente ferma il lettore per riposizionarlo
    volatile uint32_t endPtr;    // il lettore non va oltre questo blocco (fine del titolo che sta seguendo)
    int failCount;               // letture fallite di fila (al terzo tentativo la stanza si chiude)
    uint32_t histFrom;           // la storia dell'anello vale solo da questo blocco (prima del riposizionamento contiene altro)
    uint32_t ra;                 // blocchi che il lettore puo' leggere in anticipo
    int wd;                      // id del watchdog del lettore (-1 = nessuno)
    volatile uint32_t maxReadMs; // chunk piu' lento letto dalla SD dall'ultimo registro
    volatile uint64_t tkRead, tkIdle; // tick passati a leggere (SD + decifratura) e ad aspettare che il mittente avanzi
    volatile uint64_t blocksRead;     // blocchi letti in totale
    volatile uint32_t nReads;         // letture fatte in totale
    volatile uint32_t rdHist[5];      // durata delle letture: <5, 5-10, 10-20, 20-50, >=50 ms
    char err[96];
} reader_t;

static int open_title(ts_ctx* c, cia_stream* s, int* cur, const ts_manifest* m, int ti, char* err, size_t esz) {
    if(*cur == ti) {
        return 0;
    }

    if(*cur >= 0) {
        cia_stream_close(s);
        *cur = -1;
    }

    if(cia_stream_open(s, m->t[ti].tid, c->ctrRoot, c->rk, c->certsDb, c->certsDbSize) != 0) {
        snprintf(err, esz, "%.80s", s->err);
        return -1;
    }

    if(s->front.total != m->t[ti].size) {
        snprintf(err, esz, "il titolo e' cambiato durante l'invio");
        return -2;
    }

    *cur = ti;
    return 0;
}

// Primo blocco da saltare con indice maggiore di g (o UINT32_MAX).
static uint32_t next_skip_start(const ts_range* r, int n, uint32_t g) {
    uint32_t best = UINT32_MAX;
    for(int i = 0; i < n; i++) {
        if(r[i].start > g && r[i].start < best) {
            best = r[i].start;
        }
    }

    return best;
}

static void reader_fn(void* arg) {
    reader_t* r = (reader_t*) arg;
    ts_log("lettore: avviato sul core %d", (int) svcGetProcessorID());
    while(!r->stop) {
        ts_wd_beat(r->wd, 1);
        if(r->pause) {
            r->paused = 1;
            svcSleepThread(1000000);
            continue;
        }

        r->paused = 0;
        // sendPtr puo' cadere dentro un intervallo saltato: l'occupazione dell'anello si misura dalla sua fine
        uint32_t rp = r->readPtr, sp = ts_skip_adjust(r->skip, r->nSkip, r->sendPtr);
        uint32_t adj = ts_skip_adjust(r->skip, r->nSkip, rp);
        if(adj != rp) {
            r->readPtr = adj;
            continue;
        }

        // il lettore resta al massimo `ra` blocchi davanti al mittente: il resto dell'anello e' la storia dei blocchi inviati
        if(rp >= r->endPtr || rp >= r->m->totalBlocks || rp >= sp + r->ra) {
            uint64_t ik = svcGetSystemTick();
            svcSleepThread(1000000);
            r->tkIdle += svcGetSystemTick() - ik;
            continue;
        }

        int ti;
        uint64_t off;
        size_t len;
        if(!ts_block_info(r->m, rp, &ti, &off, &len)) {
            snprintf(r->err, sizeof(r->err), "blocco fuori dal manifesto");
            r->error = 1;
            return;
        }

        uint32_t left = r->m->t[ti].firstBlock + r->m->t[ti].blocks - rp;
        uint32_t room = r->ringN - (rp % r->ringN);
        uint32_t cnt = left < 16 ? left : 16;
        if(cnt > room) {
            cnt = room;
        }

        uint32_t limit = next_skip_start(r->skip, r->nSkip, rp);
        if(limit != UINT32_MAX && cnt > limit - rp) {
            cnt = limit - rp;
        }

        uint64_t remain = r->m->t[ti].size - off;
        size_t bytes = (uint64_t) cnt * TS_BLOCK < remain ? (size_t) cnt * TS_BLOCK : (size_t) remain;

        ts_wd_beat(r->wd, 2);
        uint64_t rk0 = svcGetSystemTick();
        int rfail = open_title(r->c, r->s, &r->title, r->m, ti, r->err, sizeof(r->err)) != 0 ||
                    cia_stream_read(r->s, off, r->ring + (size_t) (rp % r->ringN) * TS_BLOCK, bytes) != 0;
        uint64_t rdk = svcGetSystemTick() - rk0;
        r->tkRead += rdk;
        r->blocksRead += cnt;
        r->nReads++;
        uint32_t rms = (uint32_t) (rdk * 1000ULL / SYSCLOCK_ARM11);
        r->rdHist[rms < 5 ? 0 : (rms < 10 ? 1 : (rms < 20 ? 2 : (rms < 50 ? 3 : 4)))]++;
        if(rms > r->maxReadMs) {
            r->maxReadMs = rms;
        }

        if(rfail) {
            if(r->err[0] == 0) {
                snprintf(r->err, sizeof(r->err), "%.80s", r->s->err);
            }

            // un errore passeggero (la SD che si ferma un istante) non chiude la stanza: 3 tentativi di fila, poi si chiude
            if(++r->failCount < 3) {
                ts_log("lettore: lettura fallita al blocco %u (%s), tentativo %d di 3", (unsigned int) rp, r->err, r->failCount);
                cia_stream_close(r->s);
                r->title = -1;
                r->err[0] = 0;
                svcSleepThread(300ULL * 1000000ULL);
                continue;
            }

            ts_log("lettore: lettura fallita %d volte di fila (%s): la stanza si chiude", r->failCount, r->err);
            r->error = 1;
            return;
        }

        r->failCount = 0;

        __sync_synchronize();
        r->readPtr = rp + cnt;
    }
}

// Riposiziona il lettore: lo ferma, imposta (se skip != NULL) gli intervalli da saltare e la posizione, lo fa ripartire. La storia dell'anello
// vale solo da `pos` in poi.
static void reader_seek(reader_t* r, const ts_range* skip, int nSkip, uint32_t pos, int wd) {
    r->paused = 0; // il valore rimasto dalla pausa precedente non vale: il lettore lo rimette a 1 solo dopo aver visto `pause`
    __sync_synchronize();
    r->pause = 1;
    __sync_synchronize();
    while(!r->paused && !r->error && !r->stop) {
        ts_wd_beat(wd, 6);
        svcSleepThread(1000000);
    }

    if(skip != NULL) {
        memcpy(r->skip, skip, sizeof(ts_range) * (size_t) nSkip);
        r->nSkip = nSkip;
    }

    r->sendPtr = pos;
    r->readPtr = ts_skip_adjust(r->skip, r->nSkip, pos);
    r->histFrom = pos;
    {
        int ti = 0;
        uint64_t off;
        size_t len;
        r->endPtr = ts_block_info(r->m, pos, &ti, &off, &len) ? r->m->t[ti].firstBlock + r->m->t[ti].blocks : r->m->totalBlocks;
    }

    __sync_synchronize();
    r->pause = 0;
}

// ---------------------------------------------------------------- invio

static uint32_t now_ms(uint64_t t0) {
    return (uint32_t) (ts_now_ms() - t0) + 1;
}

static ts_ack2 gAck; // grande: non sullo stack

// Percentuale della stanza e tempo che manca dati dall'app (sender_set_room_estimate): < 0 / < -1 = non dati, valgono quelli del modulo.
static int gEstPct = -1;
static double gEstEta = -2.0;

void sender_set_room_estimate(int pct, double etaSec) {
    gEstPct = pct;
    gEstEta = etaSec;
}

// Quale nodo UDS ha mandato il READY di ogni ricevente, l'ultimo nome buono della sua console e se ha detto di aver finito con un errore (per lo schermo:
// nessuna decisione dipende da questa tabella). Il nome si ricorda per token e non si cancella mai: una ricerca che per un momento non riesce (un nodo ripreso da
// un'altra console, un servizio che rifiuta) non deve trasformarlo in "Receiver N".
static struct {
    uint16_t token, node;
    uint8_t err, nameLogged;
    char name[32];
} gNodeOf[TS_S_MAX * 2];
static unsigned int gNodeNext;

static void note_node(uint16_t token, uint16_t node) {
    for(unsigned int i = 0; i < sizeof(gNodeOf) / sizeof(gNodeOf[0]); i++) {
        if(gNodeOf[i].token == token) {
            gNodeOf[i].node = node;
            return;
        }
    }

    unsigned int s = gNodeNext % (sizeof(gNodeOf) / sizeof(gNodeOf[0]));
    memset(&gNodeOf[s], 0, sizeof(gNodeOf[s]));
    gNodeOf[s].token = token;
    gNodeOf[s].node = node;
    gNodeNext++;
}

static int slot_of(uint16_t token) {
    for(unsigned int i = 0; i < sizeof(gNodeOf) / sizeof(gNodeOf[0]); i++) {
        if(gNodeOf[i].token == token) {
            return (int) i;
        }
    }

    return -1;
}

static void note_err(uint16_t token, int err) {
    int s = slot_of(token);
    if(s >= 0) {
        gNodeOf[s].err = err ? 1 : 0;
    }
}

static int err_of(uint16_t token) {
    int s = slot_of(token);
    return s >= 0 ? gNodeOf[s].err : 0;
}

static void note_name(uint16_t token, const char* name) {
    int s = slot_of(token);
    if(s >= 0 && name[0] != 0) {
        snprintf(gNodeOf[s].name, sizeof(gNodeOf[s].name), "%s", name);
    }
}

static const char* name_of(uint16_t token) {
    int s = slot_of(token);
    return s >= 0 ? gNodeOf[s].name : "";
}

static uint16_t node_of(uint16_t token) {
    for(unsigned int i = 0; i < sizeof(gNodeOf) / sizeof(gNodeOf[0]); i++) {
        if(gNodeOf[i].token == token) {
            return gNodeOf[i].node;
        }
    }

    return 0;
}

// Svuota la coda di ricezione. I READY registrano il ricevente (cio' che vuole e lo spazio che ha), gli ACK2 dicono cosa gli manca.
static void drain(udsBindContext* bind, uint8_t* rx, ts_sched* sc, const ts_manifest* m, uint32_t now) {
    for(;;) {
        size_t actual = 0;
        u16 src = 0;
        Result pr = udsPullPacket(bind, rx, UDS_DATAFRAME_MAXSIZE, &actual, &src);
        if(R_FAILED(pr)) {
            static unsigned int errs;
            if(++errs <= 10) {
                ts_log("udsPullPacket errore 0x%08x", (unsigned int) pr);
            }

            return;
        }

        if(actual == 0) {
            return;
        }

        uint8_t type, flags;
        uint32_t session;
        uint16_t token;
        if(!ts_parse_hdr(rx, actual, &type, &flags, &session, &token) || session != m->session || token == 0) {
            continue;
        }

        if(type == TS_T_READY) {
            int cnt = 0;
            uint8_t bits[TS_BITMAP_BYTES];
            if(!ts_unpack_bitmap(rx, actual, &cnt, bits) || cnt != m->count) {
                continue;
            }

            uint16_t oldTok = 0;
            uint32_t freeMiB = ts_unpack_free_mib(rx, actual);
            int rc = sched_ready(sc, token, ts_hdr_ident(rx), bits, freeMiB, now, &oldTok);
            if(rc == -2) {
                continue; // READY in ritardo di chi ha appena salutato
            }

            if(rc >= 0) {
                note_node(token, src);
            }

            if(rc >= 0 && flags > 0) {
                sched_set_window(sc, token, (uint32_t) flags * TS_WIN_UNIT); // la finestra del ricevente (blocchi): oltre inst + finestra non gli si invia
            }

            if(oldTok != 0) {
                ts_log("ricevente %04x: stessa console del %04x, che e' stata riavviata: la voce vecchia si toglie subito", token, oldTok);
            }

            if(rc < 0) {
                static unsigned int full;
                if(++full <= 5) {
                    ts_log("ricevente %04x rifiutato: stanza piena", token);
                }
            } else if(rc == 1) {
                int nw = 0;
                for(int i = 0; i < m->count; i++) {
                    nw += ts_bit(bits, i);
                }

                ts_log("ricevente %04x entra nella stanza: vuole %d titoli, spazio libero %s", token, nw, freeMiB == 0xFFFFFFFFu ? "n.d." : "dichiarato");
                if(freeMiB != 0xFFFFFFFFu) {
                    ts_log("    spazio libero del ricevente %04x: %u MiB", token, (unsigned int) freeMiB);
                }
            }
        } else if(type == TS_T_ACK2) {
            if(ts_unpack_ack2(rx, actual, &gAck)) {
                if(sched_ack(sc, token, now, &gAck)) {
                    note_node(token, src); // (a receiver that came back on another node is right within one acknowledgement)
                    note_err(token, (flags & TS_F_ERR) != 0);
                }
            }
        } else if(type == TS_T_BYE) {
            int idx = sched_find(sc, token);
            int wasDone = idx >= 0 && sched_finished(sc, &sc->cl[idx]); // finito = ogni titolo che voleva e' installato (non lo stato degli altri)
            if(sched_bye(sc, token, now)) {
                ts_log("ricevente %04x esce (%s): slot libero", token, wasDone ? (err_of(token) ? "ha finito, con errori" : "ha finito") : "annullato");
            }
        }
    }
}

// Un frame di controllo (manifesto, stato, riepilogo). Con la coda della radio piena di blocchi veniva scartato in silenzio (errore "occupato"): chi era
// appena entrato aspettava il manifesto a lungo, e i riceventi vedevano il riepilogo (nomi, stato, tempi) ogni tre secondi invece che ogni secondo. Si
// riprova finche' la coda lo accetta (al massimo ~40 ms).
static Result send_ctl(const uint8_t* frame, size_t n) {
    Result r = 0;
    for(int tries = 0; tries < 40; tries++) {
        r = udsSendTo(UDS_BROADCAST_NETWORKNODEID, TS_DATA_CHANNEL, UDS_SENDFLAG_Default, frame, n);
        if(r != (Result) 0xC86113F0) {
            break;
        }

        svcSleepThread(1000000);
    }

    return r;
}

static void send_manifest(const ts_manifest* m, uint8_t* frame) {
    int parts = ts_manifest_parts(m);
    for(int p = 0; p < parts; p++) {
        size_t n = ts_pack_manifest(frame, UDS_DATAFRAME_MAXSIZE, m, p);
        if(n > 0) {
            send_ctl(frame, n);
        }
    }
}

// ---- fornitura dei blocchi: anello del lettore (segue il flusso piu' usato), storia, ultime letture dalla SD

typedef struct {
    ts_ctx* c;
    const ts_manifest* m;
    reader_t* rd;
    ts_runcache rc;                 // ultime letture di fila dalla SD (corse da TS_SND_RUN_BLOCKS blocchi, LRU): usata solo da fetch_block, cioe' dal thread di invio
    uint8_t* runMem;                // memoria delle corse (un blocco solo)
    uint32_t rdNow;                 // `now` della richiesta in corso, per rc_read
    char rdErr[96];                 // errore dell'ultima lettura (open_title)
    int repTitle;                   // titolo aperto nel flusso delle ripetizioni
    uint32_t repFailUntil;
    int repFailCount;
    uint64_t nOpenFail, nPaused;    // tentativi di lettura della cache non fatti: apertura del titolo fallita / pausa dopo un errore (riepilogo)
    struct {
        uint32_t next, hits, last;  // flussi di blocchi consecutivi: prossimo atteso, quanti di fila, ultimo uso
    } sm[4];
    uint32_t lastSeek, nSeeks;
    double ringRate;                // quota recente dei blocchi serviti dall'anello (media mobile): il lettore si sposta solo se l'anello serve poco
    uint64_t nRing, nCache, nSd, tkSd, tkWaitReader;
    int wd;
} fetch_t;

// Il blocco g e' nell'anello e non e' ancora stato sovrascritto (con un margine per la lettura in corso)?
static const uint8_t* ring_slot(const reader_t* rd, uint32_t g) {
    uint32_t rp = rd->readPtr;
    if(g >= rd->histFrom && g < rp && (uint64_t) g + rd->ringN > (uint64_t) rp + 2 * TS_RUN_BLOCKS) {
        return rd->ring + (size_t) (g % rd->ringN) * TS_BLOCK;
    }

    return NULL;
}

// Lettura di una corsa dal CIA virtuale per la cache (ts_rc_fill). Un titolo che non si riesce a rileggere non si riprova a ogni richiesta
// (ogni tentativo apre la cartella e rilegge il TMD): fino a repFailUntil non si tenta (TS_RC_NOOP, dst non toccato).
static int rc_read(void* ctx, const ts_rc_req* rq, uint8_t* dst) {
    fetch_t* F = (fetch_t*) ctx;
    int rc = TS_RC_NOOP;
    ts_wd_beat(F->wd, 31);
    uint64_t tkQ = svcGetSystemTick();
    if(F->rdNow < F->repFailUntil) {
        F->nPaused++; // NOOP: dst non toccato
    } else if(open_title(F->c, &gRepair, &F->repTitle, F->m, rq->title, F->rdErr, sizeof(F->rdErr)) != 0) {
        F->nOpenFail++; // NOOP: dst non toccato
    } else {
        rc = cia_stream_read(&gRepair, rq->off, dst, rq->bytes) == 0 ? TS_RC_OK : TS_RC_ERR; // ERR: dst puo' essere stato scritto a meta'
    }

    F->tkSd += svcGetSystemTick() - tkQ;
    return rc;
}

static const uint8_t* fetch_block(fetch_t* F, uint32_t g, uint32_t now) {
    reader_t* rd = F->rd;
    const ts_manifest* m = F->m;

    // flussi: il blocco continua uno dei flussi noti? (il lettore si sposta sul piu' usato)
    int si = -1;
    for(int i = 0; i < 4; i++) {
        if(F->sm[i].next == g && F->sm[i].hits > 0) {
            si = i;
            break;
        }
    }

    if(si < 0) {
        si = 0;
        for(int i = 1; i < 4; i++) {
            if(F->sm[i].last < F->sm[si].last) {
                si = i;
            }
        }

        F->sm[si].hits = 0;
    }

    F->sm[si].next = g + 1;
    F->sm[si].hits++;
    F->sm[si].last = now;

    const uint8_t* p = ring_slot(rd, g);
    if(p == NULL && !rd->pause && g >= rd->readPtr && g < rd->endPtr && g <= rd->sendPtr + 2) {
        // il flusso del lettore: e' appena dietro, lo si aspetta
        uint64_t wk = svcGetSystemTick();
        for(int w = 0; w < 200 && g >= rd->readPtr && !rd->error && !rd->pause; w++) {
            svcSleepThread(200000);
        }

        F->tkWaitReader += svcGetSystemTick() - wk;
        p = ring_slot(rd, g);
    }

    F->ringRate *= 0.97;
    if(p != NULL) {
        F->ringRate += 0.03;
        F->nRing++;
        if(g + 1 > rd->sendPtr) {
            rd->sendPtr = g + 1;
        }

        return p;
    }

    // le corse lette dalla SD: il blocco e' in una qualsiasi (la ricerca le guarda tutte; usare una corsa la rende la piu' recente)
    const uint8_t* hit = ts_rc_find(&F->rc, g);
    if(hit != NULL) {
        F->nCache++;
        return hit;
    }

    // lettura dalla SD a gruppi di blocchi vicini (stesso titolo), al posto della corsa meno usata di recente
    F->rdNow = now;
    F->rdErr[0] = 0;
    ts_rc_info ri;
    const uint8_t* run = ts_rc_fill(&F->rc, g, &ri);
    if(ri.how == TS_RC_RANGE) {
        return NULL;
    }

    int ti = ri.title;
    if(run == NULL) {
        if(now >= F->repFailUntil) {
            F->repFailUntil = now + 2000;
            F->repFailCount++;
            ts_log("lettura SD: titolo %d non rileggibile (%s), tentativo %d di 3, riprovo fra 2 s", ti, F->rdErr[0] ? F->rdErr : gRepair.err, F->repFailCount);
            if(F->repFailCount >= 3 && !rd->error) {
                snprintf(rd->err, sizeof(rd->err), "ripetizioni: %.70s", F->rdErr[0] ? F->rdErr : gRepair.err);
                rd->error = 1; // dopo 3 tentativi la stanza si chiude (come per il lettore)
            }
        }

        return NULL;
    }

    F->repFailCount = 0;
    F->nSd++;
    uint32_t cnt = ri.count;

    // un flusso di blocchi consecutivi che l'anello non segue: il lettore si sposta li' (al massimo ogni 1,5 s)
    if(F->sm[si].hits >= 8 && F->ringRate < 0.5 && (F->nSeeks == 0 || now - F->lastSeek >= 1500 || rd->readPtr >= rd->endPtr)) {
        uint32_t pos = g + cnt;
        if(pos < m->t[ti].firstBlock + m->t[ti].blocks) {
            reader_seek(rd, NULL, 0, pos, F->wd);
            F->lastSeek = now;
            F->nSeeks++;
            ts_log("lettore: segue il flusso dal blocco %u (titolo %d), spostamento %u", (unsigned int) pos, ti, (unsigned int) F->nSeeks);
        }
    }

    return run;
}

static uint32_t union_count(const ts_sched* sc, int count) {
    uint8_t u[TS_BITMAP_BYTES];
    memset(u, 0, sizeof(u));
    for(int i = 0; i < TS_S_MAX; i++) {
        if(sc->cl[i].used) {
            for(int b = 0; b < TS_BITMAP_BYTES; b++) {
                u[b] |= sc->cl[i].want[b];
            }
        }
    }

    uint32_t n = 0;
    for(int t = 0; t < count; t++) {
        n += (uint32_t) ts_bit(u, t);
    }

    return n;
}

// Percentuali e stime per lo schermo e per il riepilogo ai riceventi (definizioni: common/sched.h). Aggiorna i campi di avanzamento di *st.
static void progress_fill(const ts_sched* sc, const ts_manifest* m, uint32_t now, ts_sender_stats* st) {
    udsConnectionStatus conn;
    const int haveConn = R_SUCCEEDED(udsGetConnectionStatus(&conn)); // (the nodes that are in the network now: one question for all the receivers)
    uint64_t sumNeed = 0, sumInst = 0;
    int nReg = 0, nFin = 0;
    double worst = -1.0;
    const int curTitle = sched_cur(sc); // (the title that is being sent: whoever does not need it waits for its turn)
    st->nClients = 0;
    for(int i = 0; i < TS_S_MAX && st->nClients < TS_MAX_CLIENTS; i++) {
        const ts_sclient* cl = &sc->cl[i];
        if(!cl->used) {
            continue;
        }

        int k = st->nClients++;
        uint32_t need = sched_need_blocks(sc, cl);
        int fin = sched_finished(sc, cl);
        uint32_t age = (now >= cl->lastHeard ? now - cl->lastHeard : 0) / 100;
        st->cursor[k] = cl->installed;
        st->pctDown[k] = (uint8_t) sched_pct_down(sc, cl);
        st->pctInst[k] = (uint8_t) sched_pct_inst(sc, cl);
        st->ageDs[k] = (uint16_t) (age > 65535 ? 65535 : age);
        st->done[k] = (uint8_t) fin;
        st->cEta[k] = sched_eta_sec(sc, cl, now);
        {
            st->cname[k][0] = 0;
            st->cNode[k] = 0;
            const uint16_t node = node_of(cl->token);
            Result rNode = (Result) 0xFFFFFFFF, rName = (Result) 0xFFFFFFFF; // (what the questions answered: 0xFFFFFFFF = not asked)
            // only a node that is still in the network, and that no receiver heard more recently claims: a console that left leaves its node to the next one
            int nodeOk = node != 0 && node <= 16 && (!haveConn || ((conn.node_bitmask >> (node - 1)) & 1));
            for(int j = 0; nodeOk && j < TS_S_MAX; j++) {
                if(j != i && sc->cl[j].used && node_of(sc->cl[j].token) == node && sc->cl[j].lastHeard > cl->lastHeard) {
                    nodeOk = 0;
                }
            }

            if(nodeOk) {
                st->cNode[k] = (uint8_t) node; // (the other receivers get it in the summary and ask the name themselves: it stays while the console is in the network)
                // the name is asked only of a receiver heard lately: for a silent one the node may already be another console's; it keeps the name it had
                if(age * 100 <= TS_S_LAG_MS) {
                    udsNodeInfo ni;
                    char un[32];
                    memset(un, 0, sizeof(un));
                    rNode = udsGetNodeInformation(node, &ni);
                    if(R_SUCCEEDED(rNode)) {
                        rName = udsGetNodeInfoUsername(&ni, un);
                        if(R_SUCCEEDED(rName)) {
                            note_name(cl->token, un);
                        }
                    }
                }
            }

            snprintf(st->cname[k], sizeof(st->cname[k]), "%s", name_of(cl->token));
            // a receiver heard just now that still has no name: say why, once (the log tells what the screens cannot)
            if(st->cname[k][0] == 0 && cl->acked && age <= 10) {
                int s = slot_of(cl->token);
                if(s >= 0 && !gNodeOf[s].nameLogged) {
                    gNodeOf[s].nameLogged = 1;
                    ts_log("ricevente %04x senza nome: nodo %u, nodo valido %d, nella rete (maschera 0x%04x, stato %d), NodeInformation 0x%08x, nome 0x%08x", cl->token, (unsigned int) node, nodeOk,
                           haveConn ? (unsigned int) conn.node_bitmask : 0u, haveConn ? (int) conn.status : -1, (unsigned int) rNode, (unsigned int) rName);
                }
            }

            memcpy(st->cwant[k], cl->want, TS_BITMAP_BYTES);
            uint64_t clientDown = 0, clientInst = 0;
            for(int t = 0; t < m->count && t < TS_MAX_TITLES; t++) {
                uint32_t bl = m->t[t].blocks;
                uint32_t rem = cl->remain[t] > bl ? bl : cl->remain[t];
                uint32_t in = cl->inst[t] > bl ? bl : cl->inst[t];
                st->cTitleDown[k][t] = (uint8_t) (bl ? (uint64_t) (bl - rem) * 100 / bl : 0);
                st->cTitleInst[k][t] = (uint8_t) (bl ? (uint64_t) in * 100 / bl : 0);
                if(ts_bit(cl->want, t)) {
                    clientDown += bl - rem;
                    clientInst += in;
                }
            }

            st->cDownBlocks[k] = (uint32_t) (clientDown > 0xFFFFFFFFu ? 0xFFFFFFFFu : clientDown);
            st->cInstBlocks[k] = (uint32_t) (clientInst > 0xFFFFFFFFu ? 0xFFFFFFFFu : clientInst);

            st->cFreeMiB[k] = cl->freeMiB;
            st->cEff[k] = (uint16_t) (cl->eff > 1000 ? 1000 : cl->eff);
        }

        if(!cl->acked) {
            st->cstate[k] = 4;
        } else if(fin) {
            st->cstate[k] = err_of(cl->token) ? 6 : 2; // finished: with or without a title that failed (and finished stays finished: a console that is done and goes quiet is not "silent")
        } else if(age * 100 > TS_S_LAG_MS) {
            st->cstate[k] = 3;
        } else if(sched_remain_blocks(sc, cl) == 0) {
            st->cstate[k] = 1;
        } else if(curTitle >= 0 && curTitle < TS_MAX_TITLES && !(ts_bit(cl->want, curTitle) && cl->remain[curTitle] > 0 && !(cl->flags[curTitle] & (TS_SF_DONE | TS_SF_ERROR)))) {
            st->cstate[k] = 5; // it still needs blocks, but not of the title that is being sent: it waits for its turn
        } else {
            st->cstate[k] = 0;
        }

        st->state[k] = (st->cstate[k] == 2 || st->cstate[k] == 6) ? 3 : (st->cstate[k] == 3 ? 2 : 0);
        nReg++;
        nFin += fin;
        sumNeed += need;
        sumInst += fin ? need : (cl->installed < need ? cl->installed : need);
        if(!fin && st->cEta[k] > worst) {
            worst = st->cEta[k];
        }
    }

    int p = sumNeed ? (int) (sumInst * 100 / sumNeed) : 0;
    st->pctRoom = (nReg > 0 && nFin == nReg) ? 100 : (p > 99 ? 99 : p);
    st->roomEta = nReg > 0 && nFin == nReg ? 0.0 : worst;
}

// Strumentazione (solo registro, nessuna decisione cambia): molteplicita' REALE dei frame DATA inviati con successo = quanti riceventi vivi avevano ancora bisogno del blocco
// (sched_need_count, sola lettura). Istogramma cumulativo in 4 classi (0, 1, 2, 3 o piu' riceventi) e media; media10s = media nell'intervallo dall'ultima riga.
static void log_mult(const uint32_t n[4], uint32_t sum, uint32_t* lastN, uint32_t* lastSum, int final) {
    uint32_t tot = n[0] + n[1] + n[2] + n[3];
    uint32_t dN = tot - *lastN, dS = sum - *lastSum;
    ts_log("molteplicita: %sn0=%u n1=%u n2=%u n3p=%u frame=%u media=%.2f media10s=%.2f", final ? "finale " : "", (unsigned int) n[0], (unsigned int) n[1], (unsigned int) n[2],
           (unsigned int) n[3], (unsigned int) tot, tot ? (double) sum / (double) tot : 0.0, dN ? (double) dS / (double) dN : 0.0);
    *lastN = tot;
    *lastSum = sum;
}

int sender_run(ts_ctx* c, ts_manifest* m, int (*ui)(void* arg, const ts_sender_stats* st), void* uiArg) {
    int wdMain = -1;
    ts_sender_stats st;
    memset(&st, 0, sizeof(st));
    memset(gNodeOf, 0, sizeof(gNodeOf)); // (the table of an earlier room says nothing about this one)
    gNodeNext = 0;
    gEstPct = -1;
    gEstEta = -2.0;
    st.total = m->totalBlocks;
    st.etaSec = -1;
    st.curTitle = -1;

    udsNetworkStruct ns;
    udsBindContext bind;
    udsGenerateDefaultNetworkStruct(&ns, TS_WLANCOMM_ID, 0, TS_NET_NODES);
    Result r = udsCreateNetwork(&ns, TS_PASSPHRASE, sizeof(TS_PASSPHRASE), &bind, TS_DATA_CHANNEL, TS_HOST_RECV_BUF);
    if(R_FAILED(r)) {
        st.phase = 3;
        snprintf(st.msg, sizeof(st.msg), "rete non creata (0x%08" PRIx32 ")", (uint32_t) r);
        ui(uiArg, &st);
        return -1;
    }

    ts_log_open("mittente");
    {
        u8 chan = 0;
        if(R_SUCCEEDED(udsGetChannel(&chan))) {
            st.channel = chan; // (the radio channel the firmware chose: the app shows it)
        }
    }

    catalog_log_summary();
    ts_log("rete creata, %d titoli, %u blocchi (carosello: il mittente invia cio' che serve ai riceventi, non aspetta nessuno)", m->count, (unsigned int) m->totalBlocks);
    for(int i = 0; i < m->count; i++) {
        ts_log("  titolo %d: %016llx %s%s %.1f MiB, blocchi %u + %u%s", i, (unsigned long long) m->t[i].tid, m->t[i].name, m->t[i].kind == 1 ? " (aggiornamento)" : (m->t[i].kind == 2 ? " (DLC)" : ""),
               (double) m->t[i].size / 1048576.0, (unsigned int) m->t[i].firstBlock, (unsigned int) m->t[i].blocks, m->t[i].hasSeed ? " seed" : "");
    }
    wdMain = ts_wd_add("invio");

    ts_appdata ad;
    memset(&ad, 0, sizeof(ad));
    ad.magic = TS_APPDATA_MAGIC;
    ad.titles = (uint8_t) m->count;
    for(int i = 0; i < m->count; i++) {
        ad.totalBytes += m->t[i].size;
    }

    snprintf(ad.firstCode, sizeof(ad.firstCode), "%.15s", m->t[0].name);
    snprintf(ad.firstName, sizeof(ad.firstName), "%s", m->t[0].name);
    udsSetApplicationData(&ad, sizeof(ad));

    ts_sched* sc = sched_create(m);
    if(sc == NULL) {
        ts_log("memoria insufficiente per lo schedulatore");
        ts_log_close();
        udsDestroyNetwork();
        udsUnbind(&bind);
        return -1;
    }

    ts_power_begin_transfer();
    uint8_t* rx = malloc(UDS_DATAFRAME_MAXSIZE);
    uint8_t* frame = malloc(UDS_DATAFRAME_MAXSIZE);
    static fetch_t F;
    memset(&F, 0, sizeof(F));
    F.repTitle = -1;
    F.wd = -1;
    static reader_t rd;
    memset(&rd, 0, sizeof(rd));
    rd.wd = -1;
    rd.ring = alloc_ring(&rd.ringN);
    uint32_t nRuns = 0;
    F.runMem = alloc_runs(&nRuns);
    ts_log_corrections((int) nRuns); // dichiara quale codice produce questo registro (corse effettive dopo alloc_runs)
    int result = -1;
    Thread th = NULL;

    if(rx == NULL || frame == NULL || F.runMem == NULL || rd.ring == NULL) {
        if(rd.ring != NULL && F.runMem == NULL) {
            // le corse si allocano dopo l'anello: se non ne entrano nemmeno 2 lo si dice (prima non c'era alcun messaggio)
            ts_log("memoria insufficiente per la cache delle ripetizioni (nemmeno 2 corse da %d blocchi); heap libero %.1f MiB", TS_SND_RUN_BLOCKS, (double) ts_mem_heap_free() / 1048576.0);
            st.phase = 3;
            snprintf(st.msg, sizeof(st.msg), "memoria insufficiente (cache ripetizioni)");
            ui(uiArg, &st);
        }

        goto cleanup;
    }

    uint64_t t0 = ts_now_ms();

    rd.ra = read_ahead(rd.ringN);
    ts_log("anello in RAM: %u blocchi (%.1f MiB): %u di lettura anticipata, storia per le ripetizioni %u blocchi", (unsigned int) rd.ringN, (double) rd.ringN * TS_BLOCK / 1048576.0,
           (unsigned int) rd.ra, (unsigned int) (rd.ringN - rd.ra - 16));
    {
        // prova della velocita' di decifratura software (e' la parte dura del lettore): 4 MiB
        const size_t bsz = 1 << 20;
        uint8_t* b = malloc(bsz);
        if(b != NULL) {
            memset(b, 0x5A, bsz);
            uint8_t ctr[16] = {0};
            uint64_t a0 = ts_now_ms();
            for(int k = 0; k < 4; k++) {
                aes_ctr_crypt(c->rk, ctr, 0, b, bsz);
            }

            uint64_t adt = ts_now_ms() - a0;
            ts_log("prova AES software: %.1f MiB/s (4 MiB in %llu ms)", adt ? 4000.0 / (double) adt : 0.0, (unsigned long long) adt);
            free(b);
        }
    }

    rd.pause = 1;
    rd.wd = ts_wd_add("lettore");
    rd.m = m;
    rd.c = c;
    rd.s = &gFresh;
    rd.title = -1;
    F.c = c;
    F.m = m;
    F.rd = &rd;
    F.wd = wdMain;
    if(ts_rc_init(&F.rc, m, F.runMem, nRuns, TS_SND_RUN_BLOCKS, rc_read, &F) != 0) {
        ts_log("cache ripetizioni: parametri non validi (%u corse da %d blocchi)", (unsigned int) nRuns, TS_SND_RUN_BLOCKS);
        goto cleanup;
    }

    ts_log("cache ripetizioni: %u corse da %d blocchi (%.0f KiB), sostituzione LRU; heap libero dopo anello e corse %.1f MiB", (unsigned int) nRuns, TS_SND_RUN_BLOCKS,
           (double) ts_rc_mem_size(nRuns, TS_SND_RUN_BLOCKS) / 1024.0, (double) ts_mem_heap_free() / 1048576.0);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    th = threadCreate(reader_fn, &rd, 64 * 1024, prio + 1, c->isNew ? 2 : -2, false);
    if(th == NULL) {
        th = threadCreate(reader_fn, &rd, 64 * 1024, prio + 1, -2, false);
    }

    if(th == NULL) {
        st.phase = 3;
        snprintf(st.msg, sizeof(st.msg), "thread del lettore non creato");
        ui(uiArg, &st);
        goto cleanup;
    }

    st.phase = 0;
    uint32_t lastUi = 0, lastLog = 0, lastRoster = 0, lastApt = 0, lastManifest = 0, lastState = 0, lastDrain = 0;
    uint32_t lastNodeCheck = 0, joinHoldUntil = 0, joinBoostUntil = 0, holdCooldownUntil = 0;
    int prevNodes = 0;
    uint64_t rateFrames = 0;
    double rateEma = 0; // frame al secondo, media mobile
    uint32_t sendErrors = 0;
    uint64_t busyCount = 0, sentFrames = 0, lastBusy = 0, lastSent = 0, errCodes[4] = {0, 0, 0, 0}, errCounts[4] = {0, 0, 0, 0};
    uint64_t tkSend = 0, tkPace = 0, tkDrain = 0, nSend = 0, maxSendTk = 0, tkLog = svcGetSystemTick(), tkNoWork = 0;
    uint64_t lastRdTk = 0, lastRdIdle = 0, lastRdBlocks = 0, lastFread = 0, lastAes = 0, lastBytesRd = 0, lastNRing = 0, lastNCache = 0, lastNSd = 0, lastTkSd = 0, lastTkWait = 0;
    uint32_t lastRh[5] = {0, 0, 0, 0, 0}, sendHist[5] = {0, 0, 0, 0, 0};
    uint32_t multN[4] = {0, 0, 0, 0}, multSum = 0, multLastN = 0, multLastSum = 0, lastMult = 0; // molteplicita' reale dei frame inviati (log_mult)
    const uint64_t kS02 = SYSCLOCK_ARM11 / 5000, kS05 = SYSCLOCK_ARM11 / 2000, kS1 = SYSCLOCK_ARM11 / 1000, kS5 = SYSCLOCK_ARM11 / 200; // 0,2 / 0,5 / 1 / 5 ms in tick
    uint32_t lastRdReads = 0;
    int lastTitleLogged = -1, announced = -2;
    uint64_t tStart = ts_now_ms();
    result = -2;
    ts_log("stanza aperta: %d titoli, %u blocchi, fino a %d riceventi, canale UDS %d", m->count, (unsigned int) m->totalBlocks, TS_NET_NODES - 1, st.channel);

    // Passo di invio: di base libero (nessuna pausa). I modi fissi si scelgono con Y a stanza vuota.
    ts_pace pace;
    pace_init(&pace, gPaceMode);
    uint64_t paceTicks = (uint64_t) ((double) SYSCLOCK_ARM11 / pace.rate);
    ts_log("passo di invio: %s, %.0f blocchi/s", pace_mode_name(pace.mode), pace.rate);
    uint64_t nextTx = svcGetSystemTick();

    for(;;) {
        uint32_t now = now_ms(t0);
        ts_wd_beat(wdMain, 1);
        if(now - lastApt >= 100) {
            lastApt = now;
            if(!ts_power_poll()) {
                snprintf(st.msg, sizeof(st.msg), "il sistema chiede di chiudere");
                break;
            }
        }

        // la coda UDS si legge solo se l'evento dice che c'e' qualcosa (o ogni 5 ms): una richiesta IPC a nwm per ogni frame costa piu' del frame
        uint64_t tkD = svcGetSystemTick();
        if(svcWaitSynchronization(bind.event, 0) == 0 || now - lastDrain >= 5) {
            lastDrain = now;
            drain(&bind, rx, sc, m, now);
        }

        tkDrain += svcGetSystemTick() - tkD;
        // Una console nuova nella rete: per mezzo secondo i blocchi si fermano (la radio e' libera per il suo ingresso) e per un secondo il
        // manifesto si ripete ogni 200 ms (cinque manifesti: perderli tutti di fila e' difficile; se capita, il successivo parte comunque dopo
        // un secondo), cosi' chi entra in una stanza che sta gia' trasmettendo vede subito l'elenco, non dopo mezzo minuto. Il secondo del manifesto
        // ricomincia a ogni nodo nuovo; la pausa dei blocchi no: al piu' una ogni due secondi, cosi' tante console che entrano di fila non
        // fermano la stanza.
        if(now - lastNodeCheck >= 100) {
            lastNodeCheck = now;
            udsConnectionStatus ncs;
            if(R_SUCCEEDED(udsGetConnectionStatus(&ncs))) {
                int nodes = ncs.total_nodes > 0 ? (int) ncs.total_nodes - 1 : 0;
                if(nodes > prevNodes) {
                    joinBoostUntil = now + 1000;
                    if(now >= holdCooldownUntil) {
                        joinHoldUntil = now + 500;
                        holdCooldownUntil = now + 2000;
                    }

                    lastManifest = 0; // il manifesto parte subito
                    ts_log("nodo nuovo nella rete (%d): manifesto ogni 200 ms per 1 s (5 manifesti), blocchi fermi per 0,5 s (non piu' di una volta ogni 2 s)", nodes);
                }

                prevNodes = nodes;
            }
        }

        ts_wd_beat(wdMain, 2);
        {
            uint16_t gone[TS_S_MAX];
            int ng = sched_expire(sc, now, gone, TS_S_MAX);
            for(int i = 0; i < ng && i < TS_S_MAX; i++) {
                ts_log("ricevente %04x non si fa sentire da %d s: esce dalla stanza", gone[i], TS_S_LOST_MS / 1000);
            }
        }

        // La stanza e' sempre visibile: il manifesto si ripete, cosi' chi entra sa cosa e' offerto.
        if(lastManifest == 0 || now - lastManifest >= (now < joinBoostUntil ? 200u : 1000u)) {
            lastManifest = now;
            send_manifest(m, frame);
        }

        int serving = sched_serving(sc, now);
        // titolo in corso per i riceventi (STATE): ogni 500 ms e subito quando cambia
        if(sc->cur != announced || now - lastState >= 500) {
            lastState = now;
            if(sc->cur != announced) {
                announced = sc->cur;
                if(sc->cur >= 0) {
                    ts_log("titolo in corso: %d (%.40s, %.1f MiB), riceventi serviti %d, cambi di titolo %u", sc->cur, m->t[sc->cur].name, (double) m->t[sc->cur].size / 1048576.0, serving, (unsigned int) sc->st.switches);
                } else {
                    ts_log("titolo in corso: nessuno (nessun ricevente ha bisogno di niente)");
                }
            }

            size_t sn = ts_pack_state(frame, UDS_DATAFRAME_MAXSIZE, m->session, sc->cur < 0 ? TS_NO_TITLE : (uint8_t) sc->cur, sc->gen, (uint8_t) serving);
            if(sn > 0) {
                send_ctl(frame, sn);
            }
        }

        st.phase = serving > 0 ? 1 : 0;
        if(st.phase == 0 && (st.msg[0] == 0 || strncmp(st.msg, "stanza aperta", 13) == 0)) {
            snprintf(st.msg, sizeof(st.msg), "stanza aperta: aspetto i riceventi");
        } else if(st.phase == 1 && strncmp(st.msg, "stanza aperta", 13) == 0) {
            st.msg[0] = 0;
        }

        // Riepilogo per i riceventi: chi il mittente sente e da quanto. Chi scopre di non essere sentito si ricollega.
        if(now - lastRoster >= 1000) {
            double dtr = lastRoster == 0 ? 1.0 : (double) (now - lastRoster) / 1000.0;
            lastRoster = now;
            double inst = (double) (sentFrames - rateFrames) / dtr;
            rateFrames = sentFrames;
            rateEma = rateEma == 0 ? inst : 0.85 * rateEma + 0.15 * inst;
            st.mibps = inst * TS_BLOCK / 1048576.0;

            ts_roster ro;
            memset(&ro, 0, sizeof(ro));
            progress_fill(sc, m, now, &st);
            ro.senderNext = (uint32_t) (gEstPct >= 0 ? gEstPct : st.pctRoom);
            {
                const double eta = gEstEta >= -1.0 ? gEstEta : st.roomEta;
                ro.roomEtaSec = eta < 0 ? 0xFFFF : (uint16_t) (eta > 65534.0 ? 65534 : eta);
            }
            ro.rateKiBs = (uint32_t) (rateEma * TS_BLOCK / 1024.0);
            ro.senderMs = (uint32_t) ts_log_ms();
            for(int i = 0; i < TS_S_MAX && ro.n < 16; i++) {
                if(sc->cl[i].used) {
                    uint32_t age = (now >= sc->cl[i].lastHeard ? now - sc->cl[i].lastHeard : 0) / 100;
                    ro.token[ro.n] = sc->cl[i].token;
                    ro.ageDs[ro.n] = (uint16_t) (age > 65535 ? 65535 : age);
                    ro.cursor[ro.n] = (uint32_t) sched_pct_inst(sc, &sc->cl[i]); // percentuale installata, sui titoli che quel ricevente vuole
                    ro.node[ro.n] = st.cNode[ro.n]; // (st e' stato riempito da progress_fill nello stesso ordine degli slot)
                    ro.state[ro.n] = st.cstate[ro.n];
                    ro.down[ro.n] = st.pctDown[ro.n];
                    ro.n++;
                }
            }

            ro.ext = 1;
            if(ro.n != st.nClients) {
                ts_log("riepilogo: %d riceventi nel frame, %d nelle statistiche", ro.n, st.nClients); // (non deve succedere: i due cicli sono gli stessi)
            }

            size_t rn = ts_pack_roster(frame, UDS_DATAFRAME_MAXSIZE, m->session, &ro);
            if(rn > 0) {
                send_ctl(frame, rn);
            }
        }

        if(rd.error) {
            snprintf(st.msg, sizeof(st.msg), "lettura: %.80s", rd.err);
            break;
        }

        ts_wd_beat(wdMain, 3);
        uint32_t g;
        if(serving > 0 && now >= joinHoldUntil && sched_next(sc, now, &g)) {
            int ti;
            uint64_t off;
            size_t len;
            ts_block_info(m, g, &ti, &off, &len);
            const uint8_t* payload = fetch_block(&F, g, now);
            if(payload != NULL) {
                // il blocco si copia nel frame subito: il lettore potrebbe sovrascrivere lo slot dell'anello durante l'attesa del passo
                size_t n = ts_pack_data(frame, UDS_DATAFRAME_MAXSIZE, m->session, g, payload, len);
                // ritmo: non si invia piu' veloci del passo stabilito
                uint64_t tk = svcGetSystemTick();
                if(tk < nextTx) {
                    double ns = (double) (nextTx - tk) * 1e9 / (double) SYSCLOCK_ARM11;
                    if(ns > 30000.0) { // sotto i 30 us si gira a vuoto, sopra si dorme (sul core 0 un'attesa lunga a vuoto affama il lettore)
                        svcSleepThread((s64) ns);
                    } else {
                        while(svcGetSystemTick() < nextTx) {
                        }
                    }
                }

                tkPace += svcGetSystemTick() - tk;

                ts_wd_beat(wdMain, 4);
                uint64_t tkS = svcGetSystemTick();
                Result sr = udsSendTo(UDS_BROADCAST_NETWORKNODEID, TS_DATA_CHANNEL, UDS_SENDFLAG_Default, frame, n);
                uint64_t dtkS = svcGetSystemTick() - tkS;
                tkSend += dtkS;
                nSend++;
                if(dtkS > maxSendTk) {
                    maxSendTk = dtkS;
                }

                sendHist[dtkS < kS02 ? 0 : (dtkS < kS05 ? 1 : (dtkS < kS1 ? 2 : (dtkS < kS5 ? 3 : 4)))]++;
                if(R_SUCCEEDED(sr)) {
                    sentFrames++;
                    sendErrors = 0; // contano solo gli errori di fila
                    uint64_t after = svcGetSystemTick();
                    nextTx += paceTicks;
                    if(after > nextTx + 8 * paceTicks) {
                        nextTx = after; // dopo una pausa non si recupera a raffica
                    }

                    {
                        // strumentazione (dopo l'aggiornamento del passo): quanti riceventi vivi avevano ancora bisogno di questo blocco = molteplicita' reale del frame.
                        // Sola lettura, O(8), stesso `now` di sched_next (stessi riscontri): non cambia nessuna decisione.
                        uint32_t mu = (uint32_t) sched_need_count(sc, g, now);
                        multN[mu > 3 ? 3 : mu]++;
                        multSum += mu;
                    }
                } else if(sr == 0xC86113F0) {
                    busyCount++;
                    sched_unsend(sc, g); // la coda del chip e' piena: il blocco non e' partito
                    svcSleepThread(100000);
                } else {
                    sched_unsend(sc, g);
                    int slot = -1;
                    for(int e = 0; e < 4; e++) {
                        if(errCounts[e] != 0 && errCodes[e] == (uint32_t) sr) {
                            slot = e;
                        }
                    }

                    if(slot < 0) {
                        for(int e = 0; e < 4 && slot < 0; e++) {
                            if(errCounts[e] == 0) {
                                slot = e;
                                errCodes[e] = (uint32_t) sr;
                            }
                        }
                    }

                    if(slot >= 0 && errCounts[slot]++ == 0) {
                        ts_log("udsSendTo errore NUOVO 0x%08" PRIx32 " al blocco %u", (uint32_t) sr, (unsigned int) g);
                    }

                    svcSleepThread(1000000);
                    if(++sendErrors > 5000) {
                        snprintf(st.msg, sizeof(st.msg), "troppi errori di invio (0x%08" PRIx32 ")", (uint32_t) sr);
                        break;
                    }
                }
            } else {
                sched_unsend(sc, g); // lettura non riuscita: il blocco si riproporra'
                svcSleepThread(1000000);
            }
        } else {
            // niente da inviare: nessun ricevente ha bisogno di nulla, o tutto cio' che manca e' appena stato inviato
            uint64_t nk = svcGetSystemTick();
            svcSleepThread(serving > 0 ? 1000000 : 5000000); // stanza vuota: si controlla la rete ogni 5 ms (a batteria)
            tkNoWork += svcGetSystemTick() - nk;
        }

        ts_wd_beat(wdMain, 5);
        if(now - lastLog >= (serving > 0 ? 2000u : 30000u)) { // a stanza vuota si scrive di rado: il registro ha un limite
            double dts = lastLog == 0 ? 2.0 : (double) (now - lastLog) / 1000.0;
            lastLog = now;
            udsConnectionStatus lcs;
            int okc = R_SUCCEEDED(udsGetConnectionStatus(&lcs));
            ts_sched_stats ss;
            sched_stats(sc, &ss);
            uint64_t sumFr = 0;
            for(int t = 0; t < m->count; t++) {
                sumFr += sc->fr[t];
            }

            ts_log("stato: titolo %d gen %u serviti %d | marcia massima %llu blocchi | inviati/s %.0f occupato/s %.0f | scelte %u turno %u urgenti %u cambi %u (rimasugli %u isteresi %u) a vuoto %u | anello %llu cache %llu SD %llu | errori invio %u | nodi %d stato 0x%x | lettore max %u ms | passo %.0f/s (%s)",
                   sc->cur, (unsigned int) sc->gen, serving, (unsigned long long) sumFr, (double) (sentFrames - lastSent) / dts, (double) (busyCount - lastBusy) / dts, (unsigned int) ss.picks,
                   (unsigned int) ss.turnPicks, (unsigned int) ss.urgentPicks, (unsigned int) ss.switches, (unsigned int) ss.tinySwitches, (unsigned int) ss.dwellSwitches, (unsigned int) ss.idle,
                   (unsigned long long) F.nRing, (unsigned long long) F.nCache, (unsigned long long) F.nSd, (unsigned int) sendErrors, okc ? (int) lcs.total_nodes : -1, okc ? (unsigned int) lcs.status : 0,
                   (unsigned int) rd.maxReadMs, pace.rate, pace_mode_name(pace.mode));
            {
                // dove va il tempo del ciclo di invio: radio (SendTo), attesa del passo, SD per le letture fuori anello, lettura dei riscontri
                uint64_t tkNow = svcGetSystemTick();
                double per = (double) (tkNow - tkLog);
                if(per < 1.0) {
                    per = 1.0;
                }

                double usSend = nSend ? (double) tkSend / (double) nSend * 1e6 / (double) SYSCLOCK_ARM11 : 0.0;
                ts_log("    tempo: radio %.0f%% (SendTo medio %.0f us, max %.1f ms, %llu frame) attesa passo %.0f%% SD fuori anello %.0f%% (%llu letture) attesa lettore %.0f%% lettura riscontri %.0f%% | lettore occupato %.0f%% (%.2f MiB/s mentre legge, %u letture, max %u ms) in attesa %.0f%% | spostamenti del lettore %u",
                       100.0 * (double) tkSend / per, usSend, (double) maxSendTk * 1000.0 / (double) SYSCLOCK_ARM11, (unsigned long long) nSend, 100.0 * (double) tkPace / per,
                       100.0 * (double) (F.tkSd - lastTkSd) / per, (unsigned long long) (F.nSd - lastNSd), 100.0 * (double) (F.tkWaitReader - lastTkWait) / per, 100.0 * (double) tkDrain / per,
                       100.0 * (double) (rd.tkRead - lastRdTk) / per,
                       (rd.tkRead > lastRdTk) ? (double) (rd.blocksRead - lastRdBlocks) * TS_BLOCK / 1048576.0 / ((double) (rd.tkRead - lastRdTk) / (double) SYSCLOCK_ARM11) : 0.0,
                       (unsigned int) (rd.nReads - lastRdReads), (unsigned int) rd.maxReadMs, 100.0 * (double) (rd.tkIdle - lastRdIdle) / per, (unsigned int) F.nSeeks);
                {
                    // il lettore in dettaglio: quanto tempo in SD e quanto in decifratura software, e come si distribuiscono le letture
                    uint64_t dF = gFresh.tkFread - lastFread, dA = gFresh.tkAes - lastAes, dB = gFresh.bytesRead - lastBytesRd;
                    ts_log("    lettore: SD %.0f%% del tempo (%.1f MiB/s), decifratura %.0f%% (%.1f MiB/s), letture <5 ms: %u, 5-10: %u, 10-20: %u, 20-50: %u, >=50: %u | SendTo <0.2 ms: %u, 0.2-0.5: %u, 0.5-1: %u, 1-5: %u, >=5: %u | senza nulla da inviare %.0f%% | anello %llu cache %llu",
                           100.0 * (double) dF / per, dF ? (double) dB / 1048576.0 / ((double) dF / (double) SYSCLOCK_ARM11) : 0.0, 100.0 * (double) dA / per,
                           dA ? (double) dB / 1048576.0 / ((double) dA / (double) SYSCLOCK_ARM11) : 0.0, (unsigned int) (rd.rdHist[0] - lastRh[0]), (unsigned int) (rd.rdHist[1] - lastRh[1]),
                           (unsigned int) (rd.rdHist[2] - lastRh[2]), (unsigned int) (rd.rdHist[3] - lastRh[3]), (unsigned int) (rd.rdHist[4] - lastRh[4]), (unsigned int) sendHist[0],
                           (unsigned int) sendHist[1], (unsigned int) sendHist[2], (unsigned int) sendHist[3], (unsigned int) sendHist[4], 100.0 * (double) tkNoWork / per,
                           (unsigned long long) (F.nRing - lastNRing), (unsigned long long) (F.nCache - lastNCache));
                    lastFread = gFresh.tkFread;
                    lastAes = gFresh.tkAes;
                    lastBytesRd = gFresh.bytesRead;
                    for(int h = 0; h < 5; h++) {
                        lastRh[h] = rd.rdHist[h];
                        sendHist[h] = 0;
                    }

                    tkNoWork = 0;
                }

                lastRdTk = rd.tkRead;
                lastRdIdle = rd.tkIdle;
                lastRdBlocks = rd.blocksRead;
                lastRdReads = rd.nReads;
                lastNRing = F.nRing;
                lastNCache = F.nCache;
                lastNSd = F.nSd;
                lastTkSd = F.tkSd;
                lastTkWait = F.tkWaitReader;
                tkSend = tkPace = tkDrain = nSend = maxSendTk = 0;
                tkLog = tkNow;
            }

            lastSent = sentFrames;
            lastBusy = busyCount;
            rd.maxReadMs = 0;
            for(int i = 0; i < TS_S_MAX; i++) {
                const ts_sclient* cl = &sc->cl[i];
                if(!cl->used) {
                    continue;
                }

                char rem[160];
                int rl = 0;
                rem[0] = 0;
                for(int t = 0; t < m->count && rl < (int) sizeof(rem) - 24; t++) {
                    if(ts_bit(cl->want, t)) {
                        rl += snprintf(rem + rl, sizeof(rem) - (size_t) rl, " t%d:%u%s", t, (unsigned int) cl->remain[t], (cl->flags[t] & TS_SF_INSTALLING) ? "*" : ((cl->flags[t] & TS_SF_ACCEPT) ? "" : "!"));
                    }
                }

                ts_log("    ricevente %04x: eta' riscontro %u ms, installati %u blocchi, efficienza %u%%, spazio %s%u MiB, mancano:%s%s", cl->token,
                       (unsigned int) (now >= cl->lastHeard ? now - cl->lastHeard : 0), (unsigned int) cl->installed, (unsigned int) (cl->eff / 10),
                       cl->freeMiB == 0xFFFFFFFFu ? "n.d. " : "", cl->freeMiB == 0xFFFFFFFFu ? 0u : (unsigned int) cl->freeMiB, rem, cl->acked ? "" : " (nessun riscontro ancora)");
            }

            if(lastMult == 0 || now - lastMult >= 9000) {
                lastMult = now;
                log_mult(multN, multSum, &multLastN, &multLastSum, 0);
            }
        }

        if(now - lastUi >= 250) {
            lastUi = now;
            uint64_t sumFr = 0;
            for(int t = 0; t < m->count; t++) {
                sumFr += sc->fr[t];
            }

            st.next = (uint32_t) sumFr;
            st.repairs = sentFrames > sumFr ? sentFrames - sumFr : 0;
            st.paceRate = pace.rate;
            st.paceLossPct = 0.0;
            st.paceMode = pace.mode;
            st.curTitle = sc->cur;
            if(st.curTitle >= 0 && st.curTitle != lastTitleLogged) {
                lastTitleLogged = st.curTitle;
                (void) tStart;
            }

            progress_fill(sc, m, now, &st);
            st.etaSec = st.roomEta;
            {
                udsConnectionStatus ucs;
                st.connected = R_SUCCEEDED(udsGetConnectionStatus(&ucs)) && ucs.total_nodes > 0 ? ucs.total_nodes - 1 : 0;
                st.ready = sched_alive(sc, now);
                st.wanted = (int) union_count(sc, m->count);
                st.lap = sc->gen;
                st.nInline = serving;
                st.nLate = 0;
            }

            if(ui(uiArg, &st) == TS_UI_CANCEL) {
                snprintf(st.msg, sizeof(st.msg), "sala chiusa");
                result = 0;
                break;
            }
        }
    }

    st.phase = result == 0 ? 2 : 3;

    {
        double secs = (double) (ts_now_ms() - tStart) / 1000.0;
        ts_sched_stats ss;
        sched_stats(sc, &ss);
        uint64_t sumFr = 0;
        for(int t = 0; t < m->count; t++) {
            sumFr += sc->fr[t];
        }

        ts_log("riepilogo invio: durata %.1f s, frame inviati %llu (%.2f MiB/s medi), marcia massima %llu blocchi, ripetizioni %llu, scelte %u (turno %u, urgenti %u), cambi di titolo %u, anello %llu cache %llu SD %llu, spostamenti del lettore %u",
               secs, (unsigned long long) sentFrames, secs > 0 ? (double) sentFrames * TS_BLOCK / 1048576.0 / secs : 0.0, (unsigned long long) sumFr,
               (unsigned long long) (sentFrames > sumFr ? sentFrames - sumFr : 0), (unsigned int) ss.picks, (unsigned int) ss.turnPicks, (unsigned int) ss.urgentPicks, (unsigned int) ss.switches,
               (unsigned long long) F.nRing, (unsigned long long) F.nCache, (unsigned long long) F.nSd, (unsigned int) F.nSeeks);
        ts_log("cache ripetizioni: %u corse, hit %llu, corse lette %llu (%llu blocchi), letture fallite %llu, aperture fallite %llu, in pausa %llu (nessuna lettura per 2 s dopo un errore)",
               (unsigned int) F.rc.nSlots, (unsigned long long) F.rc.hits, (unsigned long long) F.rc.fills, (unsigned long long) F.rc.blocksRead, (unsigned long long) F.rc.failed,
               (unsigned long long) F.nOpenFail, (unsigned long long) F.nPaused);
        log_mult(multN, multSum, &multLastN, &multLastSum, 1);
    }

    ui(uiArg, &st);

    for(int i = 0; i < 10; i++) {
        size_t n = ts_pack_simple(frame, UDS_DATAFRAME_MAXSIZE, TS_T_END, 0, m->session, 0);
        udsSendTo(UDS_BROADCAST_NETWORKNODEID, TS_DATA_CHANNEL, UDS_SENDFLAG_Default, frame, n);
        svcSleepThread(30ULL * 1000000ULL);
    }

cleanup:
    if(th != NULL) {
        rd.stop = 1;
        threadJoin(th, U64_MAX);
        threadFree(th);
    }

    ts_wd_remove(rd.wd);
    ts_wd_remove(wdMain);
    ts_power_end_transfer();

    cia_stream_close(&gFresh);
    cia_stream_close(&gRepair);
    sched_destroy(sc);
    free(rx);
    free(frame);
    free(F.runMem);
    free(rd.ring);
    ts_log("fine invio: risultato %d (%s)", result, st.msg);
    {
        int wdEnd = ts_wd_add("chiusura");
        ts_wd_beat(wdEnd, 1);
        ts_log("chiusura: distruggo la rete...");
        ts_log_flush();
        udsDestroyNetwork();
        ts_wd_beat(wdEnd, 2);
        ts_log("chiusura: rete distrutta, rilascio il collegamento...");
        ts_log_flush();
        udsUnbind(&bind);
        ts_wd_beat(wdEnd, 3);
        ts_log("chiusura: rete rilasciata, torno al menu");
        ts_log_flush();
        ts_wd_remove(wdEnd);
    }

    ts_log_close();
    return result;
}
