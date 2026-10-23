#include "store.h"

#include <stdlib.h>
#include <string.h>

#ifdef __3DS__
#include <3ds.h>
typedef LightLock st_mutex;
#define ST_MUTEX_INIT(m) LightLock_Init(m)
#define ST_LOCK(m) LightLock_Lock(m)
#define ST_UNLOCK(m) LightLock_Unlock(m)
#else
typedef int st_mutex; // le prove su PC girano su un solo thread
#define ST_MUTEX_INIT(m) ((void) (m))
#define ST_LOCK(m) ((void) (m))
#define ST_UNLOCK(m) ((void) (m))
#endif

#define ST_MARGIN_BYTES (64ULL * 1024 * 1024) // spazio che non si tocca mai
#define ST_BEYOND_MAX 131072u // al massimo ~190 MiB parcheggiati oltre la finestra per titolo attivo (i buchi lunghi non devono riempire la SD e il file)

// Oltre la finestra del titolo attivo (cioe' PARKOK nel riepilogo e accettazione in store_offer) si parcheggia solo con la coda del parcheggio sotto il 75% SE
// TS_PARKOK_SOTTO_75 e' 1 (di base e' 0: si parcheggia fino a coda piena; make PARKOK75=1 per accendere la soglia).
// Si conta l'occupazione dell'anello (qtail - qhead, anche le voci gia' prese che stanno dietro a una piu' vecchia ancora viva): e' quella che riempie la coda
// e fa scartare (store_offer scarta con qtail - qhead >= qcap), quindi e' quella giusta da guardare, non il numero delle voci vive.
// Decisione del 14.10 (opzionale di IMPL1): azzera gli scartati per coda piena, ma con un parcheggio lento il ricevente indietro smette di accettare
// il fronte del flusso e i primi riceventi finiscono dopo (INTEG1-NOTE §5.4 e §10): per questo di base e' SPENTA (TS_PARKOK_SOTTO_75=0, e' il comportamento di IMPL1
// senza l'opzionale); make PARKOK75=1 la accende.
#if TS_PARKOK_SOTTO_75 // (definito in store.h: 0 di base)
#define ST_PARK_Q_OK(s) ((s)->qtail - (s)->qhead < (s)->qcap / 4 * 3)
#else
#define ST_PARK_Q_OK(s) 1
#endif

typedef struct {
    uint32_t idx, count;
    uint64_t off;
} st_run;

typedef struct {
    int wanted;
    int state;
    int accept;
    int fileOpen;
    uint32_t first, blocks;
    uint64_t size;
    uint8_t* present;   // bitmap dei blocchi sul file
    uint8_t* held;      // bitmap dei blocchi sul file o in coda di parcheggio (cio' che si dichiara disponibile)
    uint32_t nPresent;
    uint32_t hi;        // 1 + indice piu' alto visto (finestra, coda o file)
    uint32_t instNext;  // primo blocco locale non ancora consegnato all'installazione
    st_run* runs;       // ordinati per idx, senza sovrapposizioni
    uint32_t nRuns, capRuns;
    uint64_t parkedBytes;
    int allocated;      // 1 dopo la prima scrittura in AM del titolo attivo: AM ha gia' preso tutto il suo spazio
    uint64_t reclaim;   // spazio che si libera all'attivazione (versione vecchia da sovrascrivere), al massimo `size`
} st_title;

typedef struct {
    uint32_t g;
    uint32_t at;  // istante d'arrivo (ms, ts_fileio.now_ms): la scrittura parte dalla voce piu' vecchia
    uint16_t len;
} st_slot;

struct ts_store {
    const ts_manifest* m;
    st_title t[TS_MAX_TITLES];
    int active;
    ts_win* win;
    ts_fileio io;
    st_mutex lock;
    // coda di parcheggio
    uint8_t* qdata;
    uint8_t* pbuf;
    st_slot* qmeta;
    uint32_t qcap, qhead, qtail; // indici che crescono
    int lastTitle;
    uint32_t beyondCount;      // blocchi del titolo attivo parcheggiati oltre la finestra (tetto ST_BEYOND_MAX: oltre si scarta)
    int starved;               // 1 = installatore a secco: oltre la finestra il titolo attivo parcheggia (vedi store_set_starved)
    volatile int parkingTitle; // titolo di cui il thread di parcheggio sta scrivendo un tratto sul file (-1 = nessuno): store_finish lo aspetta prima di cancellare il file
    // politica di scrittura del parcheggio (vedi store_park_step)
    uint32_t arrCnt, rateT, arrRate; // blocchi accodati dall'ultima stima, istante dell'ultima stima, ritmo d'arrivo in blocchi/s (media mobile)
    int rateOk;
    // tratto che il parcheggio sta scrivendo sul file (fuori dal lock): i suoi blocchi non sono piu' in coda e non sono ancora sul file; store_next_run non li cerca in coda
    int flightTitle;           // -1 = nessuno
    uint32_t flightG0, flightN; // primo blocco (globale) e numero di blocchi
    int idle;                  // 1 = il mittente non ha niente da inviare (STATE senza titolo)
    uint32_t idleSince;
    ts_store_stats st;
    uint64_t lastFree;
};

// Orologio dello store in ms (0 se l'ambiente non ne da' uno: allora l'eta' delle voci non conta). La funzione dell'ambiente non tocca lo store.
static inline uint32_t st_now(const ts_store* s) {
    return s->io.now_ms ? s->io.now_ms(s->io.ctx) : 0;
}

// ---------------------------------------------------------------- bitmap

static inline int bget(const uint8_t* b, uint32_t i) {
    return (b[i >> 3] >> (i & 7)) & 1;
}

static inline void bclr(uint8_t* b, uint32_t i) {
    b[i >> 3] &= (uint8_t) ~(1u << (i & 7));
}

static inline void bset(uint8_t* b, uint32_t i) {
    b[i >> 3] |= (uint8_t) (1u << (i & 7));
}

// Primo indice in [from, to) con bit = v (to se non c'e'), a parole di 32 bit.
static uint32_t bnext(const uint8_t* b, uint32_t from, uint32_t to, int v) {
    uint32_t i = from;
    while(i < to && (i & 31)) {
        if(bget(b, i) == v) {
            return i;
        }

        i++;
    }

    uint32_t skip = v ? 0u : 0xFFFFFFFFu; // parola da saltare: tutta senza il bit cercato
    while(i + 32 <= to) {
        uint32_t w = (uint32_t) b[i >> 3] | ((uint32_t) b[(i >> 3) + 1] << 8) | ((uint32_t) b[(i >> 3) + 2] << 16) | ((uint32_t) b[(i >> 3) + 3] << 24);
        if(w != skip) {
            break;
        }

        i += 32;
    }

    while(i < to) {
        if(bget(b, i) == v) {
            return i;
        }

        i++;
    }

    return to;
}

static uint32_t bcount(const uint8_t* b, uint32_t from, uint32_t to) {
    uint32_t n = 0, i = from;
    while(i < to && (i & 7)) {
        n += (uint32_t) bget(b, i++);
    }

    // ARMv6 non ha POPCNT: a parole di 32 bit con il conteggio a somme parallele (e non un byte per volta con __popcountsi2)
    while(i + 32 <= to) {
        uint32_t w = (uint32_t) b[i >> 3] | ((uint32_t) b[(i >> 3) + 1] << 8) | ((uint32_t) b[(i >> 3) + 2] << 16) | ((uint32_t) b[(i >> 3) + 3] << 24);
        w = w - ((w >> 1) & 0x55555555u);
        w = (w & 0x33333333u) + ((w >> 2) & 0x33333333u);
        w = (w + (w >> 4)) & 0x0F0F0F0Fu;
        n += (w * 0x01010101u) >> 24;
        i += 32;
    }

    while(i + 8 <= to) {
        n += (uint32_t) __builtin_popcount(b[i >> 3]);
        i += 8;
    }

    while(i < to) {
        n += (uint32_t) bget(b, i++);
    }

    return n;
}

// ---------------------------------------------------------------- creazione

ts_store* store_create(const ts_manifest* m, const uint8_t* want, const ts_fileio* io, ts_win* win, uint32_t parkSlots) {
    ts_store* s = calloc(1, sizeof(*s));
    if(s == NULL) {
        return NULL;
    }

    s->m = m;
    s->io = *io;
    s->win = win;
    s->active = -1;
    s->parkingTitle = -1;
    s->flightTitle = -1;
    s->lastTitle = 0;
    s->rateT = st_now(s);
    ST_MUTEX_INIT(&s->lock);
    s->qcap = parkSlots;
    s->qdata = malloc((size_t) parkSlots * TS_BLOCK);
    s->qmeta = calloc(parkSlots, sizeof(st_slot));
    s->pbuf = malloc((size_t) ST_MAX_RUN * TS_BLOCK);
    if(s->qdata == NULL || s->qmeta == NULL || s->pbuf == NULL) {
        store_destroy(s);
        return NULL;
    }

    for(int i = 0; i < m->count; i++) {
        st_title* t = &s->t[i];
        t->first = m->t[i].firstBlock;
        t->blocks = m->t[i].blocks;
        t->size = m->t[i].size;
        t->wanted = ts_bit(want, i) && t->blocks > 0;
        t->accept = 1;
        if(t->wanted) {
            t->present = calloc(((size_t) t->blocks + 7) / 8 + 4, 1);
            t->held = calloc(((size_t) t->blocks + 7) / 8 + 4, 1);
            if(t->present == NULL || t->held == NULL) {
                store_destroy(s);
                return NULL;
            }
        }
    }

    return s;
}

void store_destroy(ts_store* s) {
    if(s == NULL) {
        return;
    }

    for(int i = 0; i < TS_MAX_TITLES; i++) {
        free(s->t[i].present);
        free(s->t[i].held);
        free(s->t[i].runs);
    }

    free(s->qdata);
    free(s->qmeta);
    free(s->pbuf);
    free(s);
}

// ---------------------------------------------------------------- titolo di un blocco

static int title_of(ts_store* s, uint32_t g) {
    const ts_manifest* m = s->m;
    int c = s->lastTitle;
    if(c < m->count && g >= m->t[c].firstBlock && g < m->t[c].firstBlock + m->t[c].blocks) {
        return c;
    }

    for(int i = 0; i < m->count; i++) {
        if(g >= m->t[i].firstBlock && g < m->t[i].firstBlock + m->t[i].blocks) {
            s->lastTitle = i;
            return i;
        }
    }

    return -1;
}

// ---------------------------------------------------------------- rete

int store_offer(ts_store* s, uint32_t g, const uint8_t* payload, size_t len) {
    if(len > TS_BLOCK) {
        return ST_OFFER_IGNORE;
    }

    ST_LOCK(&s->lock);
    s->st.offered++;
    int ti = title_of(s, g);
    if(ti < 0 || !s->t[ti].wanted || s->t[ti].state == ST_DONE || s->t[ti].state == ST_ERROR) {
        s->st.ignored++;
        ST_UNLOCK(&s->lock);
        return ST_OFFER_IGNORE;
    }

    st_title* T = &s->t[ti];
    uint32_t i = g - T->first;
    size_t expect = (i + 1 == T->blocks) ? (size_t) (T->size - (uint64_t) i * TS_BLOCK) : TS_BLOCK;
    if(len != expect) {
        s->st.ignored++; // lunghezza diversa da quella attesa: frame sbagliato
        ST_UNLOCK(&s->lock);
        return ST_OFFER_IGNORE;
    }

    if(ti == s->active && i < T->instNext) {
        s->st.dup++;
        ST_UNLOCK(&s->lock);
        return ST_OFFER_DUP;
    }

    if(bget(T->present, i)) {
        s->st.dup++;
        ST_UNLOCK(&s->lock);
        return ST_OFFER_DUP;
    }

    int beyond = 0; // 1 = blocco del titolo attivo oltre la finestra che si parcheggia perche' l'installatore e' a secco
    if(ti == s->active) {
        int r = win_put(s->win, g, payload, len);
        if(r == 1) {
            if(i + 1 > T->hi) {
                T->hi = i + 1;
            }

            s->st.win++;
            ST_UNLOCK(&s->lock);
            return ST_OFFER_WIN;
        }

        if(r == 0) {
            s->st.dup++;
            ST_UNLOCK(&s->lock);
            return ST_OFFER_DUP;
        }

        // Oltre la finestra del titolo attivo NON si parcheggia: la SD e' quella su cui AM sta scrivendo, e scrivere sul file cio' che e' davanti
        // all'installazione la dimezza (prove su console 12.10: AM da 2 a 0,5 MiB/s, coda di parcheggio piena, ricevente sempre piu' indietro). Il
        // mittente conosce la finestra (READY) e l'avanzamento (ACK2 `inst`) e non invia oltre: cio' che passa comunque si rimanda piu' avanti.
        // Eccezione: se l'installatore e' a secco (aspetta un blocco che manca, AM non scrive) la SD e' libera e parcheggiare conviene: altrimenti tutto cio' che
        // passa mentre il ricevente ha un buco andrebbe rimandato (una linea instabile faceva rinviare gli stessi blocchi piu' volte).
        if(!(s->starved && T->accept && s->beyondCount < ST_BEYOND_MAX && ST_PARK_Q_OK(s))) {
            s->st.dropWin++;
            ST_UNLOCK(&s->lock);
            return ST_OFFER_DROP;
        }

        beyond = 1;
    }

    if(bget(T->held, i)) {
        s->st.dup++; // gia' in coda di parcheggio (o sul file)
        ST_UNLOCK(&s->lock);
        return ST_OFFER_DUP;
    }

    if(!T->accept) {
        s->st.dropNoAccept++;
        ST_UNLOCK(&s->lock);
        return ST_OFFER_DROP;
    }

    if(s->qtail - s->qhead >= s->qcap) {
        s->st.dropFull++;
        ST_UNLOCK(&s->lock);
        return ST_OFFER_DROP;
    }

    uint32_t slot = s->qtail % s->qcap;
    memcpy(s->qdata + (size_t) slot * TS_BLOCK, payload, len);
    s->qmeta[slot].g = g;
    s->qmeta[slot].at = st_now(s); // l'eta' decide quando il parcheggio lo scrive (store_park_step)
    s->qmeta[slot].len = (uint16_t) len;
    s->qtail++;
    s->arrCnt++;
    bset(T->held, i);
    if(beyond) {
        s->beyondCount++;
    }
    uint32_t q = s->qtail - s->qhead;
    if(q > s->st.parkPeak) {
        s->st.parkPeak = q;
    }

    if(i + 1 > T->hi) {
        T->hi = i + 1;
    }

    s->st.parked++;
    ST_UNLOCK(&s->lock);
    return ST_OFFER_PARK;
}

uint32_t store_park_queued(ts_store* s) {
    ST_LOCK(&s->lock);
    uint32_t n = s->qtail - s->qhead;
    ST_UNLOCK(&s->lock);
    return n;
}

// ---------------------------------------------------------------- parcheggio

static int add_run(st_title* T, uint32_t idx, uint32_t count, uint64_t off) {
    // posizione di inserimento (ordinato per idx)
    uint32_t lo = 0, hi = T->nRuns;
    while(lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if(T->runs[mid].idx < idx) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    // fusione con il precedente se contiguo sia nell'indice sia nel file
    if(lo > 0) {
        st_run* p = &T->runs[lo - 1];
        if(p->idx + p->count == idx && p->off + (uint64_t) p->count * TS_BLOCK == off) {
            p->count += count;
            // e con il successivo se ora combacia
            if(lo < T->nRuns) {
                st_run* n = &T->runs[lo];
                if(p->idx + p->count == n->idx && p->off + (uint64_t) p->count * TS_BLOCK == n->off) {
                    p->count += n->count;
                    memmove(n, n + 1, sizeof(st_run) * (T->nRuns - lo - 1));
                    T->nRuns--;
                }
            }

            return 1;
        }
    }

    if(lo < T->nRuns) {
        st_run* n = &T->runs[lo];
        if(idx + count == n->idx && off + (uint64_t) count * TS_BLOCK == n->off) {
            n->idx = idx;
            n->off = off;
            n->count += count;
            return 1;
        }
    }

    if(T->nRuns == T->capRuns) {
        uint32_t nc = T->capRuns ? T->capRuns * 2 : 64;
        st_run* nr = realloc(T->runs, sizeof(st_run) * nc);
        if(nr == NULL) {
            return 0; // senza memoria per il tratto: i blocchi non si segnano presenti (il mittente li rimandera')
        }

        T->runs = nr;
        T->capRuns = nc;
    }

    memmove(&T->runs[lo + 1], &T->runs[lo], sizeof(st_run) * (T->nRuns - lo));
    T->runs[lo].idx = idx;
    T->runs[lo].count = count;
    T->runs[lo].off = off;
    T->nRuns++;
    return 1;
}

static const st_run* find_run(const st_title* T, uint32_t i) {
    uint32_t lo = 0, hi = T->nRuns;
    while(lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const st_run* r = &T->runs[mid];
        if(i < r->idx) {
            hi = mid;
        } else if(i >= r->idx + r->count) {
            lo = mid + 1;
        } else {
            return r;
        }
    }

    return NULL;
}

#define ST_DEAD 0xFFFFFFFFu // slot della coda gia' preso da una scrittura (si salta quando la testa lo raggiunge)
#define ST_SCAN 4096        // quanti elementi della coda si guardano per affiancare alla voce piu' vecchia i suoi vicini (per indice)

// L'elemento (titolo t, blocco g) della coda si scarta? (lock tenuto)
static int park_stale(const ts_store* s, int t, uint32_t g) {
    return t < 0 || !s->t[t].wanted || s->t[t].state == ST_DONE || s->t[t].state == ST_ERROR || !s->t[t].accept || bget(s->t[t].present, g - s->t[t].first) ||
           (t == s->active && g - s->t[t].first < s->t[t].instNext);
}

// Eta' minima (ms) che la voce piu' vecchia della coda deve avere prima che il parcheggio la scriva: ST_AGE_MS, ma non piu' del tempo che al ritmo
// d'arrivo attuale ci vuole a riempire il 40% della coda (le code corte, p. es. 1024 blocchi sugli Old 3DS, non possono aspettare 3 s). Il ritmo
// (blocchi/s accodati) si stima su finestre di almeno 500 ms con una media mobile; una divisione per chiamata, nessuna per elemento. (lock tenuto)
static uint32_t park_min_age(ts_store* s, uint32_t now) {
    if(s->io.now_ms == NULL) {
        return 0; // senza orologio l'eta' non si puo' misurare: si scrive la testa subito
    }

    uint32_t dt = now - s->rateT;
    if(dt >= ST_RATE_MS) {
        uint32_t inst = (uint32_t) ((uint64_t) s->arrCnt * 1000u / dt);
        s->arrRate = s->rateOk ? (s->arrRate + inst) / 2 : inst;
        s->rateOk = 1;
        s->arrCnt = 0;
        s->rateT = now;
    }

    if(s->arrRate == 0) {
        return ST_AGE_MS;
    }

    uint32_t fill = (uint32_t) ((uint64_t) s->qcap * ST_AGE_FILL_PCT / 100 * 1000u / s->arrRate);
    return fill < ST_AGE_MS ? fill : ST_AGE_MS;
}

int store_park_step(ts_store* s, int* title) {
    uint8_t* buf = s->pbuf; // buffer di scrittura (solo il thread di parcheggio lo usa)
    ST_LOCK(&s->lock);
    uint32_t now = st_now(s); // letto col lock: nessuna voce in coda e' piu' recente
    int ti = -1, progressed = 0;
    uint32_t i0 = 0, n = 0;
    // 1) la testa: si scartano i blocchi gia' presenti, di titoli che non accettano piu' e quelli gia' presi
    for(;;) {
        if(s->qhead == s->qtail) {
            ST_UNLOCK(&s->lock);
            return progressed ? 1 : 0;
        }

        uint32_t slot = s->qhead % s->qcap;
        uint32_t g = s->qmeta[slot].g;
        if(g == ST_DEAD) {
            s->qhead++;
            continue;
        }

        int t = title_of(s, g);
        if(park_stale(s, t, g)) {
            if(t >= 0 && s->t[t].held != NULL && !(s->t[t].present != NULL && bget(s->t[t].present, g - s->t[t].first))) {
                bclr(s->t[t].held, g - s->t[t].first); // scartato: torna mancante
            }

            s->qhead++;
            progressed = 1;
            continue;
        }

        ti = t;
        break;
    }

    // 2) scrittura "dal piu' vecchio, con ritardo". La voce piu' vecchia viva e' la testa: si scrive il tratto contiguo (per indice) che la contiene solo
    // quando ha almeno l'eta' minima. Aspettando, i buchi (ripetizioni del mittente dopo 0,2-1 s) si riparano e i vicini arrivano, e il tratto esce lungo e
    // in ordine; la testa esce comunque sempre al piu' tardi dopo l'eta' minima, quindi nessuna voce muore di fame e la coda non cresce senza limite.
    // Eccezioni, scritte subito: (a) coda al 75% (valvola: altrimenti si scarta); (b) fine mandata (il mittente non ha piu' niente da inviare: nessun vicino
    // arrivera' piu'). L'installatore non aspetta mai il parcheggio: i blocchi che gli servono e sono ancora in coda li prende da solo (store_next_run).
    uint32_t seedSlot = s->qhead % s->qcap;
    // Eta' con segno: l'orologio e' monotono, ma se per qualunque motivo tornasse indietro (now < at) la differenza senza segno darebbe ~4,29e9 ms e sporcherebbe
    // parkAgeMax. L'eta' si conta allora 0 e la voce si scrive subito (nessuna voce resta ad aspettare un orologio che non la raggiungera' piu').
    int32_t ageDiff = (int32_t) (now - s->qmeta[seedSlot].at);
    uint32_t headAge = ageDiff < 0 ? 0u : (uint32_t) ageDiff;
    int why = 0;
    {
        uint32_t minAge = park_min_age(s, now);
        if(headAge >= minAge || ageDiff < 0) {
            why = 1;
        } else if(s->qtail - s->qhead >= s->qcap / 4 * 3) {
            why = 2;
        } else if(s->idle && (int32_t) (now - s->idleSince) >= ST_IDLE_MS) {
            why = 3;
        }

        if(why == 0) {
            ST_UNLOCK(&s->lock);
            return progressed ? 1 : 0;
        }
    }

    {
        // Il tratto: la voce piu' vecchia g0 estesa a sinistra e poi a destra con le voci in coda dello stesso titolo, fino a ST_MAX_RUN blocchi. Si guardano i
        // primi ST_SCAN elementi della coda solo quanto serve a riempire slotOf[] per gli indici [g0 - ST_MAX_RUN, g0 + ST_MAX_RUN). Niente divisioni per
        // elemento: lo slot avanza con il giro.
        const st_title* T0 = &s->t[ti];
        uint32_t lo = T0->first, hiG = T0->first + T0->blocks;
        uint32_t g0 = s->qmeta[seedSlot].g;
        uint32_t cnt = s->qtail - s->qhead;
        if(cnt > ST_SCAN) {
            cnt = ST_SCAN;
        }

        enum { SPAN = 2 * ST_MAX_RUN };
        uint32_t base = g0 >= lo + ST_MAX_RUN ? g0 - ST_MAX_RUN : lo;
        uint32_t slotOf[SPAN];
        for(uint32_t k = 0; k < SPAN; k++) {
            slotOf[k] = 0xFFFFFFFFu;
        }

        uint32_t slot = s->qhead % s->qcap;
        for(uint32_t k = 0; k < cnt; k++) {
            uint32_t g = s->qmeta[slot].g;
            if(g != ST_DEAD && g >= base && g < base + SPAN && g < hiG && !bget(T0->present, g - lo) && !(ti == s->active && g - lo < T0->instNext)) {
                slotOf[g - base] = slot;
            }

            if(++slot == s->qcap) {
                slot = 0;
            }
        }

        // Tratto [L, R] che contiene g0. Prima a SINISTRA (fino a 63 blocchi), poi a destra fino a ST_MAX_RUN in tutto. Le voci a sinistra della piu' vecchia sono
        // riparazioni arrivate dopo: se restassero fuori andrebbero in fondo al file come blocchi isolati e spezzerebbero il tratto; quelle a destra no, perche'
        // il tratto successivo comincia proprio dopo R e si fonde con questo.
        uint32_t L = g0, R = g0;
        while(g0 - L < ST_MAX_RUN - 1 && L > base && slotOf[L - 1 - base] != 0xFFFFFFFFu) {
            L--;
        }

        while(R - L + 1 < ST_MAX_RUN && R + 1 < base + SPAN && slotOf[R + 1 - base] != 0xFFFFFFFFu) {
            R++;
        }

        n = R - L + 1;
        i0 = L - lo;
        for(uint32_t k = 0; k < n; k++) {
            uint32_t sl = slotOf[L + k - base];
            if(s->qmeta[sl].len < TS_BLOCK) {
                memset(buf + (size_t) k * TS_BLOCK, 0, TS_BLOCK); // solo l'ultimo blocco di un titolo e' corto
            }

            memcpy(buf + (size_t) k * TS_BLOCK, s->qdata + (size_t) sl * TS_BLOCK, s->qmeta[sl].len);
            s->qmeta[sl].g = ST_DEAD;
        }

        // la testa salta le voci gia' prese: lo slot avanza con il giro (niente divisione per voce, con il lock tenuto)
        uint32_t hs = s->qhead % s->qcap;
        while(s->qhead != s->qtail && s->qmeta[hs].g == ST_DEAD) {
            s->qhead++;
            if(++hs == s->qcap) {
                hs = 0;
            }
        }
    }

    if(why == 1) {
        s->st.parkByAge++;
    } else if(why == 2) {
        s->st.parkByValve++;
    } else {
        s->st.parkByIdle++;
    }

    if(s->io.now_ms != NULL && headAge > s->st.parkAgeMax) {
        s->st.parkAgeMax = headAge;
    }

    st_title* T = &s->t[ti];
    int needOpen = !T->fileOpen;
    s->parkingTitle = ti;
    s->flightTitle = ti;
    s->flightG0 = T->first + i0;
    s->flightN = n;
    ST_UNLOCK(&s->lock);

    int rc = 0;
    uint64_t off = 0;
    if(needOpen) {
        rc = s->io.open ? s->io.open(s->io.ctx, ti) : 0;
        if(rc == 0) {
            ST_LOCK(&s->lock);
            int gone = T->state == ST_DONE || T->state == ST_ERROR;
            if(!gone) {
                T->fileOpen = 1;
            }

            ST_UNLOCK(&s->lock);
            if(gone) {
                // il titolo e' finito mentre si apriva il file: nessuno lo cancellerebbe piu'
                if(s->io.remove) {
                    s->io.remove(s->io.ctx, ti);
                }

                ST_LOCK(&s->lock);
                s->parkingTitle = -1;
                s->flightTitle = -1;
                ST_UNLOCK(&s->lock);
                return 1;
            }
        }
    }

    if(rc == 0) {
        rc = s->io.append(s->io.ctx, ti, buf, (size_t) n * TS_BLOCK, &off);
    }

    ST_LOCK(&s->lock);
    s->parkingTitle = -1;
    s->flightTitle = -1; // scritto (present) o tornato mancante (held ripulito) qui sotto

    if(rc != 0) {
        if(T->held != NULL && T->present != NULL) {
            for(uint32_t k = 0; k < n; k++) {
                if(!bget(T->present, i0 + k)) {
                    bclr(T->held, i0 + k); // non scritti: tornano mancanti
                }
            }
        }

        T->accept = 0; // disco pieno o SD non scrivibile: questo titolo non si parcheggia piu'
        s->st.parkErrors++;
        ST_UNLOCK(&s->lock);
        if(title) {
            *title = ti;
        }

        return -1;
    }

    if(T->state != ST_DONE && T->state != ST_ERROR) {
        if(add_run(T, i0, n, off)) {
            for(uint32_t k = 0; k < n; k++) {
                if(!bget(T->present, i0 + k)) {
                    bset(T->present, i0 + k);
                    bset(T->held, i0 + k);
                    T->nPresent++;
                }
            }
        } else {
            for(uint32_t k = 0; k < n; k++) {
                if(!bget(T->present, i0 + k)) {
                    bclr(T->held, i0 + k); // tratto non registrato (memoria finita): il blocco resta mancante
                }
            }
        }

        T->parkedBytes += (uint64_t) n * TS_BLOCK;
        s->st.fileBlocks += n;
    }

    ST_UNLOCK(&s->lock);
    return 1;
}

// ---------------------------------------------------------------- scelta dell'attivo, installazione

int store_active(ts_store* s) {
    ST_LOCK(&s->lock);
    int a = s->active;
    ST_UNLOCK(&s->lock);
    return a;
}

int store_pick_active(ts_store* s, int curHint) {
    ST_LOCK(&s->lock);
    int pick = -1;
    if(s->active >= 0) {
        pick = s->active;
        ST_UNLOCK(&s->lock);
        return pick;
    }

    // (0) un titolo gia' tutto sul file: si installa a velocita' della SD (il piu' piccolo per primo)
    uint64_t bestSize = 0;
    for(int i = 0; i < s->m->count; i++) {
        const st_title* T = &s->t[i];
        uint64_t net = T->size - T->reclaim; // costo netto dell'installazione: la versione vecchia si libera prima di scrivere
        if(T->wanted && T->state == ST_PENDING && T->nPresent == T->blocks && (pick < 0 || net < bestSize)) {
            pick = i;
            bestSize = net;
        }
    }

    // (1) il titolo che il mittente sta inviando
    if(pick < 0 && curHint >= 0 && curHint < s->m->count && s->t[curHint].wanted && s->t[curHint].state == ST_PENDING) {
        pick = curHint;
    }

    // (2) il titolo con piu' blocchi gia' sul file
    if(pick < 0) {
        uint32_t best = 0;
        for(int i = 0; i < s->m->count; i++) {
            const st_title* T = &s->t[i];
            if(T->wanted && T->state == ST_PENDING && (pick < 0 || T->nPresent > best)) {
                pick = i;
                best = T->nPresent;
            }
        }
    }

    ST_UNLOCK(&s->lock);
    return pick;
}

void store_activate(ts_store* s, int t) {
    ST_LOCK(&s->lock);
    st_title* T = &s->t[t];
    s->active = t;
    s->starved = 0;
    s->beyondCount = 0;
    T->allocated = 0;
    T->state = ST_INSTALLING;
    T->accept = 1; // il titolo attivo si riceve sempre (nei limiti dello spazio per il parcheggio dell'eccedenza)
    win_reset(s->win, T->first + T->instNext);
    ST_UNLOCK(&s->lock);
}

int store_next_run(ts_store* s, uint8_t* out, uint32_t maxBlocks, size_t* bytes, int* src) {
    *bytes = 0;
    *src = ST_SRC_NONE;
    ST_LOCK(&s->lock);
    int a = s->active;
    if(a < 0) {
        ST_UNLOCK(&s->lock);
        return 0;
    }

    st_title* T = &s->t[a];
    uint32_t i = T->instNext;
    if(i >= T->blocks) {
        ST_UNLOCK(&s->lock);
        return 0;
    }

    uint32_t g = T->first + i;
    if(maxBlocks > ST_MAX_RUN) {
        maxBlocks = ST_MAX_RUN;
    }

    if(s->win->cursor == g) {
        uint32_t n = 0;
        size_t total = 0;
        while(n < maxBlocks && i + n < T->blocks) {
            size_t len;
            const uint8_t* p = win_peek(s->win, &len);
            if(p == NULL) {
                break;
            }

            memcpy(out + (size_t) n * TS_BLOCK, p, len);
            total += len;
            win_pop(s->win);
            n++;
        }

        if(n > 0) {
            T->instNext += n;
            *bytes = total;
            *src = ST_SRC_WIN;
            ST_UNLOCK(&s->lock);
            return (int) n;
        }
    }

    if(bget(T->present, i)) {
        const st_run* r = find_run(T, i);
        if(r == NULL) {
            ST_UNLOCK(&s->lock); // presente ma senza tratto (memoria finita al momento di registrarlo): non disponibile
            return 0;
        }

        uint32_t n = r->idx + r->count - i;
        if(n > maxBlocks) {
            n = maxBlocks;
        }

        if(n > T->blocks - i) {
            n = T->blocks - i;
        }

        uint64_t off = r->off + (uint64_t) (i - r->idx) * TS_BLOCK;
        ST_UNLOCK(&s->lock);
        int rc = s->io.read(s->io.ctx, a, off, out, (size_t) n * TS_BLOCK);
        if(rc != 0) {
            return -1;
        }

        ST_LOCK(&s->lock);
        // il cursore della finestra segue: cio' che e' arrivato dal file non deve restare nella finestra
        win_skip(s->win, g, g + n);
        T->instNext += n;
        s->st.fileReads++;
        ST_UNLOCK(&s->lock);
        size_t total = (size_t) n * TS_BLOCK;
        if(i + n == T->blocks) {
            total = total - TS_BLOCK + (size_t) (T->size - (uint64_t) (T->blocks - 1) * TS_BLOCK);
        }

        *bytes = total;
        *src = ST_SRC_FILE;
        return (int) n;
    }

    if(bget(T->held, i)) {
        // Dichiarato disponibile (held) ma non sul file: o il parcheggio lo sta scrivendo proprio ora (basta aspettare), o e' ancora nella coda di parcheggio e lo si
        // prende da li', senza che il parcheggio lo scriva e l'installazione lo rilegga dalla SD: sono i blocchi parcheggiati oltre la finestra mentre l'installatore
        // era a secco e quelli di un titolo appena attivato. Nel tratto in scrittura non si cerca, cosi' attendere la scrittura non costa scansioni a ogni giro.
        if(s->flightTitle == a && g - s->flightG0 < s->flightN) {
            ST_UNLOCK(&s->lock);
            return 0;
        }

        // Quanti blocchi di fila da i sono in coda: held e non present (invariante: held - present = voci vive in coda + blocchi in volo; il tratto in volo si
        // esclude). La scansione della coda, fatta col lock che usa anche la rete, si ferma appena li ha trovati tutti invece di scorrere tutta la coda (fino a
        // 14.592 voci) per cercarne altri che non ci sono: prima, con un buco entro 64 blocchi, scorreva sempre tutto.
        uint32_t lim = maxBlocks < T->blocks - i ? maxBlocks : T->blocks - i;
        uint32_t want = 0;
        while(want < lim && bget(T->held, i + want) && !bget(T->present, i + want)) {
            want++;
        }

        if(s->flightTitle == a && s->flightG0 > g && s->flightG0 - g < want) {
            want = s->flightG0 - g; // il tratto in scrittura comincia piu' avanti: fin li'
        }

        uint32_t slotOf[ST_MAX_RUN];
        for(uint32_t k = 0; k < want; k++) {
            slotOf[k] = 0xFFFFFFFFu;
        }

        uint32_t found = 0, cnt = s->qtail - s->qhead, slot = s->qhead % s->qcap, scanned = 0;
        while(scanned < cnt && found < want) {
            uint32_t qg = s->qmeta[slot].g;
            if(qg != ST_DEAD && qg - g < want && slotOf[qg - g] == 0xFFFFFFFFu) {
                slotOf[qg - g] = slot;
                found++;
            }

            scanned++;
            if(++slot == s->qcap) {
                slot = 0;
            }
        }

        s->st.qscanCalls++;
        s->st.qscanElems += scanned;
        if(scanned > s->st.qscanMax) {
            s->st.qscanMax = scanned;
        }

        lim = want;
        uint32_t n = 0;
        while(n < lim && slotOf[n] != 0xFFFFFFFFu) {
            n++;
        }

        if(n > 0) {
            size_t total = 0;
            for(uint32_t k = 0; k < n; k++) {
                uint32_t sl = slotOf[k];
                memcpy(out + (size_t) k * TS_BLOCK, s->qdata + (size_t) sl * TS_BLOCK, s->qmeta[sl].len);
                total += s->qmeta[sl].len;
                s->qmeta[sl].g = ST_DEAD;
                bclr(T->held, i + k); // consegnato: non e' piu' ne' in coda ne' da dichiarare
            }

            // la testa salta le voci gia' prese: lo slot avanza con il giro (niente divisione per voce, con il lock tenuto)
            uint32_t hs = s->qhead % s->qcap;
            while(s->qhead != s->qtail && s->qmeta[hs].g == ST_DEAD) {
                s->qhead++;
                if(++hs == s->qcap) {
                    hs = 0;
                }
            }

            win_skip(s->win, g, g + n); // il cursore della finestra segue, come per la lettura dal file
            T->instNext += n;
            s->st.queueBlocks += n;
            ST_UNLOCK(&s->lock);
            *bytes = total;
            *src = ST_SRC_QUEUE;
            return (int) n;
        }
    }

    ST_UNLOCK(&s->lock);
    return 0;
}

int store_active_complete(ts_store* s) {
    ST_LOCK(&s->lock);
    int done = s->active >= 0 && s->t[s->active].instNext >= s->t[s->active].blocks;
    ST_UNLOCK(&s->lock);
    return done;
}

void store_finish(ts_store* s, int t, int ok) {
    ST_LOCK(&s->lock);
    st_title* T = &s->t[t];
    T->state = ok ? ST_DONE : ST_ERROR;
    free(T->present);
    T->present = NULL;
    free(T->held);
    T->held = NULL;
    free(T->runs);
    T->runs = NULL;
    T->nRuns = T->capRuns = 0;
    T->nPresent = 0;
    T->parkedBytes = 0;
    if(s->active == t) {
        s->active = -1;
        s->starved = 0;
    }

    ST_UNLOCK(&s->lock);
#ifdef __3DS__
    // se il thread di parcheggio sta scrivendo sul file di questo titolo (un blocco che era in coda e che e' arrivato anche dalla finestra),
    // si aspetta che finisca (anche fino a un paio di minuti: la SD si ferma per 20 s): chiudere l'handle durante la scrittura la farebbe fallire
    for(int w = 0; w < 120000; w++) {
        ST_LOCK(&s->lock);
        int busy = s->parkingTitle == t;
        ST_UNLOCK(&s->lock);
        if(!busy) {
            break;
        }

        svcSleepThread(1000000);
    }
#endif
    ST_LOCK(&s->lock);
    int hadFile = T->fileOpen;
    T->fileOpen = 0;
    ST_UNLOCK(&s->lock);
    if(hadFile && s->io.remove) {
        s->io.remove(s->io.ctx, t);
    }
}

// ---------------------------------------------------------------- riscontri

// Fine della corsa di blocchi disponibili che comincia in i (i disponibile): primo non disponibile in [i, to) (to se non c'e'). (lock tenuto)
static uint32_t skip_avail_locked(const ts_store* s, int t, uint32_t i, uint32_t to) {
    const st_title* T = &s->t[t];
    uint32_t zoneLo = 0, zoneHi = 0; // zona coperta dalla finestra (indici locali)
    if(t == s->active) {
        uint32_t lo = s->win->cursor > T->first ? s->win->cursor - T->first : 0;
        zoneLo = lo;
        zoneHi = lo + s->win->W;
    }

    while(i < to) {
        if(i >= zoneLo && i < zoneHi) {
            uint32_t e = zoneHi < to ? zoneHi : to;
            uint32_t slot = (T->first + i) % s->win->W; // una divisione sola: poi lo slot avanza da se' (no `%` per blocco su ARM11)
            while(i < e && (bget(T->held, i) || s->win->have[slot])) {
                i++;
                if(++slot == s->win->W) {
                    slot = 0;
                }
            }

            if(i < e) {
                return i;
            }
        } else {
            uint32_t e = (i < zoneLo && zoneLo < to) ? zoneLo : to;
            uint32_t n = bnext(T->held, i, e, 0);
            if(n < e) {
                return n;
            }

            i = e;
        }
    }

    return to;
}

// Fine della corsa di blocchi non disponibili che comincia in i: primo disponibile in [i, to). (lock tenuto)
static uint32_t skip_missing_locked(const ts_store* s, int t, uint32_t i, uint32_t to) {
    const st_title* T = &s->t[t];
    uint32_t zoneLo = 0, zoneHi = 0;
    if(t == s->active) {
        uint32_t lo = s->win->cursor > T->first ? s->win->cursor - T->first : 0;
        zoneLo = lo;
        zoneHi = lo + s->win->W;
    }

    while(i < to) {
        if(i >= zoneLo && i < zoneHi) {
            uint32_t e = zoneHi < to ? zoneHi : to;
            uint32_t slot = (T->first + i) % s->win->W;
            while(i < e && !(bget(T->held, i) || s->win->have[slot])) {
                i++;
                if(++slot == s->win->W) {
                    slot = 0;
                }
            }

            if(i < e) {
                return i;
            }
        } else {
            uint32_t e = (i < zoneLo && zoneLo < to) ? zoneLo : to;
            uint32_t n = bnext(T->held, i, e, 1);
            if(n < e) {
                return n;
            }

            i = e;
        }
    }

    return to;
}

int store_missing(ts_store* s, int title, ts_range* out, int max) {
    if(max < 1) {
        return 0;
    }

    if(title < 0 || title >= s->m->count) {
        return 0;
    }

    ST_LOCK(&s->lock);
    st_title* T = &s->t[title];
    if(!T->wanted || T->state == ST_DONE || T->state == ST_ERROR || T->present == NULL) {
        ST_UNLOCK(&s->lock);
        return 0;
    }

    int n = 0;
    uint32_t i = T->instNext;
    uint32_t hi = T->hi < T->instNext ? T->instNext : T->hi;
    if(hi > T->blocks) {
        hi = T->blocks;
    }

    while(i < hi) {
        // primo non disponibile da i
        uint32_t ms = skip_avail_locked(s, title, i, hi);
        if(ms >= hi) {
            break;
        }

        if(n == max - 1) {
            // gli intervalli non bastano: l'ultimo copre tutto fino alla fine (si rimanda anche qualcosa che c'e', ma niente va perso)
            out[n].start = ms;
            out[n].count = T->blocks - ms;
            n++;
            ST_UNLOCK(&s->lock);
            return n;
        }

        uint32_t me = skip_missing_locked(s, title, ms, hi);
        out[n].start = ms;
        out[n].count = me - ms;
        n++;
        i = me;
    }

    // coda: tutto cio' che non e' mai stato visto
    if(hi < T->blocks) {
        if(n > 0 && out[n - 1].start + out[n - 1].count == hi) {
            out[n - 1].count = T->blocks - out[n - 1].start;
        } else if(n < max) {
            out[n].start = hi;
            out[n].count = T->blocks - hi;
            n++;
        }
    }

    ST_UNLOCK(&s->lock);
    return n;
}

int store_summary(ts_store* s, ts_sum* out, int max) {
    int k = 0;
    for(int t = 0; t < s->m->count && k < max; t++) {
        ts_range r;
        ST_LOCK(&s->lock);
        st_title* T = &s->t[t];
        int live = T->wanted && T->state != ST_DONE && T->state != ST_ERROR && T->present != NULL;
        if(!live) {
            ST_UNLOCK(&s->lock);
            continue;
        }

        // blocchi disponibili sopra instNext: sul file + in finestra
        uint32_t have = bcount(T->held, T->instNext, T->blocks);
        if(t == s->active) {
            uint32_t cur = s->win->cursor, W = s->win->W;
            uint32_t slot = cur % W; // una divisione sola per chiamata
            for(uint32_t g = cur; g < cur + W; g++) {
                if(s->win->have[slot] && g >= T->first && g < T->first + T->blocks && !bget(T->held, g - T->first)) {
                    have++;
                }

                if(++slot == W) {
                    slot = 0;
                }
            }
        }

        uint32_t left = T->blocks - T->instNext;
        out[k].title = (uint8_t) t;
        // il titolo attivo riceve sempre almeno nella finestra, anche se per la parte oltre non c'e' spazio da parcheggiare: il mittente deve continuare a inviarlo
        out[k].flags = (uint8_t) (((T->accept || t == s->active) ? ST_F_ACCEPT : 0) | ((t == s->active && s->starved && T->accept && s->beyondCount < ST_BEYOND_MAX && ST_PARK_Q_OK(s)) ? ST_F_PARKOK : 0) | (T->state == ST_INSTALLING ? ST_F_INSTALLING : 0));
        out[k].remain = have >= left ? 0 : left - have;
        out[k].inst = T->instNext;
        ST_UNLOCK(&s->lock);

        if(out[k].remain == 0) {
            out[k].first = T->blocks;
        } else if(store_missing(s, t, &r, 1) > 0) {
            out[k].first = r.start;
        } else {
            out[k].first = T->instNext; // nessun buco sotto il piu' alto visto: manca da li' in avanti
        }

        k++;
    }

    return k;
}

// ---------------------------------------------------------------- spazio

void store_mark_allocated(ts_store* s, int title) {
    if(title < 0 || title >= s->m->count) {
        return;
    }

    ST_LOCK(&s->lock);
    s->t[title].allocated = 1;
    ST_UNLOCK(&s->lock);
}

void store_set_starved(ts_store* s, int starved) {
    ST_LOCK(&s->lock);
    s->starved = starved;
    ST_UNLOCK(&s->lock);
}

void store_set_idle(ts_store* s, int idle) {
    ST_LOCK(&s->lock);
    if(idle && !s->idle) {
        s->idleSince = st_now(s);
    }

    s->idle = idle ? 1 : 0;
    ST_UNLOCK(&s->lock);
}

void store_set_reclaim(ts_store* s, int title, uint64_t bytes) {
    if(title < 0 || title >= s->m->count) {
        return;
    }

    ST_LOCK(&s->lock);
    s->t[title].reclaim = bytes < s->t[title].size ? bytes : s->t[title].size;
    ST_UNLOCK(&s->lock);
}

void store_update_space(ts_store* s, uint64_t freeBytes) {
    ST_LOCK(&s->lock);
    s->lastFree = freeBytes;
    uint64_t avail = freeBytes > ST_MARGIN_BYTES ? freeBytes - ST_MARGIN_BYTES : 0;
    // quanto spazio si prendera' ancora l'installazione attiva
    if(s->active >= 0) {
        const st_title* A = &s->t[s->active];
        // AM prepara tutto lo spazio del titolo alla prima scrittura (visto nei registri: la SD libera cala dell'intero titolo e resta li'): da quel momento
        // freeBytes lo esclude gia' e riservarlo ancora lo conterebbe due volte
        uint64_t installed = (uint64_t) A->instNext * TS_BLOCK;
        uint64_t reserve = A->allocated ? 0 : (A->size > installed ? A->size - installed : 0);
        avail = avail > reserve ? avail - reserve : 0;
    }

    // il titolo attivo parcheggia l'eccedenza oltre la finestra solo se c'e' spazio
    if(s->active >= 0) {
        s->t[s->active].accept = avail > 0 ? 1 : 0;
    }

    // titoli da parcheggiare: dal piu' piccolo; condizione: somma di cio' che ancora serve in temporaneo + il piu' piccolo (installazione del primo)
    int order[TS_MAX_TITLES];
    int cnt = 0;
    for(int i = 0; i < s->m->count; i++) {
        const st_title* T = &s->t[i];
        if(T->wanted && T->state == ST_PENDING && i != s->active) {
            order[cnt++] = i;
        }
    }

    // ordine per costo netto (dimensione meno lo spazio che l'installazione libera): il primo a essere installato e' il piu' economico
    for(int a = 1; a < cnt; a++) { // insertion sort
        int v = order[a], b = a - 1;
        uint64_t nv = s->t[v].size - s->t[v].reclaim;
        while(b >= 0 && s->t[order[b]].size - s->t[order[b]].reclaim > nv) {
            order[b + 1] = order[b];
            b--;
        }

        order[b + 1] = v;
    }

    // Condizione: lo spazio ancora da scrivere per tutti i parcheggiati + il costo netto del primo che si installera' (il suo file resta mentre AM
    // scrive, ma la versione vecchia si e' gia' liberata) entra in cio' che c'e'. I vecchi degli altri si liberano solo piu' avanti: non contano.
    uint64_t sumGrowth = 0, firstNet = 0;
    int haveFirst = 0;
    for(int a = 0; a < cnt; a++) {
        st_title* T = &s->t[order[a]];
        uint64_t growth = T->size > T->parkedBytes ? T->size - T->parkedBytes : 0;
        uint64_t net = T->size - T->reclaim;
        uint64_t first = haveFirst ? firstNet : net;
        if(sumGrowth + growth + first <= avail || T->nPresent == T->blocks) {
            T->accept = 1;
            sumGrowth += growth;
            if(!haveFirst) {
                haveFirst = 1;
                firstNet = net;
            }
        } else {
            T->accept = 0;
        }
    }

    ST_UNLOCK(&s->lock);
}

int store_state(ts_store* s, int title) {
    ST_LOCK(&s->lock);
    int v = s->t[title].state;
    ST_UNLOCK(&s->lock);
    return v;
}

int store_accepts(ts_store* s, int title) {
    ST_LOCK(&s->lock);
    int v = s->t[title].accept;
    ST_UNLOCK(&s->lock);
    return v;
}

void store_stats(ts_store* s, ts_store_stats* out) {
    ST_LOCK(&s->lock);
    *out = s->st;
    ST_UNLOCK(&s->lock);
}
