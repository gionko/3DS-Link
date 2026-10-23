#include "flow.h"

#include <stdlib.h>
#include <string.h>

// Differenza senza segno che non diventa mai enorme se l'orologio di chi chiama fa un passo indietro.
static uint32_t age_of(uint32_t now, uint32_t t) {
    return now >= t ? now - t : 0;
}

int flow_init(ts_flow* f, uint32_t total, uint32_t window, uint32_t safety) {
    memset(f, 0, sizeof(*f));
    f->total = total;
    f->W = window;
    f->Wcap = window;
    f->safety = safety;
    f->repairGapMs = 250;
    f->pend = calloc(window, 1);
    f->pc = calloc((size_t) TS_MAX_CLIENTS * window, 1);
    f->sentAt = calloc(window, sizeof(uint32_t));
    f->reqSeen = calloc(window, 1);
    if(f->pend == NULL || f->pc == NULL || f->sentAt == NULL || f->reqSeen == NULL) {
        flow_free(f);
        return -1;
    }

    return 0;
}

int flow_set_window(ts_flow* f, uint32_t w) {
    if(w == 0 || w > f->Wcap || f->next != 0) {
        return -1;
    }

    f->W = w;
    return 0;
}

void flow_free(ts_flow* f) {
    free(f->pend);
    free(f->pc);
    free(f->sentAt);
    free(f->reqSeen);
    memset(f, 0, sizeof(*f));
}

static int in_set(const ts_client* c) {
    return c->used && !c->lagging && !c->behind && !c->late;
}

// Finestra dichiarata da un ricevente; se non la si conosce si assume il minimo.
static uint32_t cap_of(const ts_client* c) {
    return c->cap != 0 ? c->cap : 4096;
}

// Cursore efficace: quello dichiarato, ma non sotto il primo blocco che gli serve.
static uint32_t eff_cursor(const ts_client* c) {
    return c->cursor > c->startHint ? c->cursor : c->startHint;
}

void flow_set_client_hint(ts_flow* f, int idx, uint32_t firstNeeded) {
    if(idx >= 0 && idx < TS_MAX_CLIENTS) {
        f->cl[idx].startHint = firstNeeded;
    }
}

int flow_inline_count(const ts_flow* f) {
    int n = 0;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        n += (f->cl[i].used && !f->cl[i].done && !f->cl[i].late);
    }

    return n;
}

int flow_late_count(const ts_flow* f) {
    int n = 0;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        n += (f->cl[i].used && !f->cl[i].done && f->cl[i].late);
    }

    return n;
}

int flow_find(const ts_flow* f, uint16_t node) {
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        if(f->cl[i].used && f->cl[i].node == node) {
            return i;
        }
    }

    return -1;
}

void flow_set_client_skips(ts_flow* f, int idx, const ts_range* r, int n) {
    if(idx < 0 || idx >= TS_MAX_CLIENTS) {
        return;
    }

    if(n > TS_MAX_TITLES + 1) {
        n = TS_MAX_TITLES + 1;
    }

    if(n > 0) {
        memcpy(f->cl[idx].cskip, r, sizeof(ts_range) * (size_t) n);
    }

    f->cl[idx].ncskip = n < 0 ? 0 : n;
}

void flow_set_client_info(ts_flow* f, int idx, uint32_t cap, int mayRide) {
    if(idx < 0 || idx >= TS_MAX_CLIENTS) {
        return;
    }

    f->cl[idx].cap = cap;
    f->cl[idx].mayRide = mayRide;
}

int flow_active(const ts_flow* f) {
    int n = 0;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        n += f->cl[i].used;
    }

    return n;
}

int flow_lost(const ts_flow* f) {
    int n = 0;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        n += (!f->cl[i].used && f->cl[i].lost);
    }

    return n;
}

int flow_behind(const ts_flow* f) {
    int n = 0;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        n += (f->cl[i].used && (f->cl[i].lagging || f->cl[i].behind));
    }

    return n;
}

void flow_forget_lost(ts_flow* f) {
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        if(!f->cl[i].used) {
            f->cl[i].lost = 0;
        }
    }
}

// Cancella le richieste di un blocco (di tutti i riceventi).
static void req_clear_slot(ts_flow* f, uint32_t slot) {
    if(f->pend[slot] == 0) {
        return;
    }

    f->pend[slot] = 0;
    f->pendCount--;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        uint8_t* p = &f->pc[(size_t) i * f->W + slot];
        if(*p) {
            *p = 0;
            if(f->cl[i].nReq > 0) {
                f->cl[i].nReq--;
            }
        }
    }
}

// Toglie tutte le richieste di un ricevente (che le ripetera' al prossimo riscontro).
static void drop_requests(ts_flow* f, int ci) {
    ts_client* c = &f->cl[ci];
    if(c->nReq == 0) {
        return;
    }

    uint8_t* row = &f->pc[(size_t) ci * f->W];
    for(uint32_t s = 0; s < f->W; s++) {
        if(row[s]) {
            row[s] = 0;
            if(f->pend[s] > 0 && --f->pend[s] == 0) {
                f->pendCount--;
            }
        }
    }

    c->nReq = 0;
    c->scanPos = 0;
}

int flow_client(ts_flow* f, uint16_t node, uint32_t now) {
    int freeSlot = -1, lostSlot = -1;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        if(f->cl[i].used && f->cl[i].node == node) {
            f->cl[i].lastHeard = now;
            return i;
        }

        if(!f->cl[i].used && f->cl[i].lost && f->cl[i].node == node) {
            // rientra: riprende dal suo cursore
            f->cl[i].used = 1;
            f->cl[i].lost = 0;
            f->cl[i].lagging = 0;
            f->cl[i].behind = 0;
            f->cl[i].lastHeard = now;
            return i;
        }

        if(!f->cl[i].used && !f->cl[i].lost && freeSlot < 0) {
            freeSlot = i;
        }

        if(!f->cl[i].used && f->cl[i].lost && lostSlot < 0) {
            lostSlot = i;
        }
    }

    if(freeSlot < 0) {
        freeSlot = lostSlot; // tabella piena: si riusa il posto di un ricevente perso
    }

    if(freeSlot < 0) {
        return -1;
    }

    drop_requests(f, freeSlot);
    ts_client* c = &f->cl[freeSlot];
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->late = 1; // entra a giro in corso: si aggancia (flow_ack) o aspetta il giro successivo (flow_update_lap)
    c->node = node;
    c->lastHeard = now;
    return freeSlot;
}

uint32_t flow_min_cursor(const ts_flow* f) {
    uint32_t m = f->next;
    int any = 0;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        // chi ha finito (cursore = fine) non limita nessuno: se un altro e' in silenzio non deve far credere che tutti siano avanti
        if(in_set(&f->cl[i]) && !f->cl[i].done) {
            if(!any || f->cl[i].cursor < m) {
                m = f->cl[i].cursor;
            }

            any = 1;
        }
    }

    if(!any) {
        // tutti in ritardo: la finestra segue comunque il piu' indietro, come con un solo ricevente
        for(int i = 0; i < TS_MAX_CLIENTS; i++) {
            if(f->cl[i].used && !f->cl[i].late) { // chi aspetta il giro successivo non conta
                if(!any || f->cl[i].cursor < m) {
                    m = f->cl[i].cursor;
                }

                any = 1;
            }
        }
    }

    return m;
}

// Il minimo e' avanzato: libera le richieste dei blocchi ormai consegnati a tutti.
static void advance_base(ts_flow* f, uint32_t newBase) {
    if(newBase <= f->base) {
        return;
    }

    uint32_t from = f->base;
    if(newBase - from > f->W) {
        from = newBase - f->W;
    }

    for(uint32_t g = from; g < newBase; g++) {
        uint32_t slot = g % f->W;
        req_clear_slot(f, slot);
        f->sentAt[slot] = 0;
        f->reqSeen[slot] = 0;
    }

    f->base = newBase;
    if(f->scan < newBase) {
        f->scan = newBase;
    }
}

static void reset_window(ts_flow* f, uint32_t newBase) {
    memset(f->pend, 0, f->W);
    memset(f->pc, 0, (size_t) TS_MAX_CLIENTS * f->W);
    memset(f->sentAt, 0, sizeof(uint32_t) * f->W);
    memset(f->reqSeen, 0, f->W);
    f->pendCount = 0;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        f->cl[i].nReq = 0;
    }

    f->base = newBase;
    f->scan = newBase;
}

// Segna come richiesti i blocchi [s0, e0) di un ricevente (ridotti alla parte valida: dentro la finestra e gia' inviati). countLoss = 1 se sono
// buchi veri (blocchi arrivati fuori ordine che mancano): contano nella perdita stimata (statReq); 0 per la coda mai vista.
static void add_req_range(ts_flow* f, int ci, uint64_t s0, uint64_t e0, int countLoss, uint32_t now) {
    ts_client* c = &f->cl[ci];
    uint8_t* row = &f->pc[(size_t) ci * f->W];
    uint64_t lo = f->base, hiLim = (uint64_t) f->base + f->W;
    if(hiLim > f->next) {
        hiLim = f->next; // solo blocchi gia' inviati
    }

    // Si riduce l'intervallo a quello valido e si scorre con uno slot che avanza: niente `%` e niente scansione dei salti per ogni
    // blocco (sull'ARM11 la divisione e' software).
    if(s0 < lo) {
        s0 = lo;
    }

    if(e0 > hiLim) {
        e0 = hiLim;
    }

    if(s0 >= e0) {
        return;
    }

    int si = 0;
    while(si < f->nSkip && (uint64_t) f->skip[si].start + f->skip[si].count <= s0) {
        si++;
    }

    uint32_t slot = (uint32_t) (s0 % f->W);
    for(uint64_t g = s0; g < e0; g++, slot = slot + 1 == f->W ? 0 : slot + 1) {
        while(si < f->nSkip && (uint64_t) f->skip[si].start + f->skip[si].count <= g) {
            si++;
        }

        if(si < f->nSkip && g >= f->skip[si].start) {
            continue; // blocco di un titolo che nessuno ha chiesto
        }

        if(row[slot]) {
            continue;
        }

        uint32_t last = f->sentAt[slot];
        if(last != 0 && age_of(now, last - 1) < f->repairGapMs) {
            continue; // riparato da poco: il blocco e' ancora in volo
        }

        row[slot] = 1;
        c->nReq++;
        if(c->nReq == 1 || (uint32_t) g < c->scanPos) {
            c->scanPos = (uint32_t) g;
        }

        if(countLoss && !f->reqSeen[slot]) {
            f->reqSeen[slot] = 1;
            f->statReq++;
        }

        if(f->pend[slot] == 0) {
            f->pendCount++;
            if((uint32_t) g < f->scan || f->pendCount == 1) {
                f->scan = (uint32_t) g;
            }
        }

        if(f->pend[slot] < 255) {
            f->pend[slot]++;
        }
    }
}

// Chi tiene ferma la finestra: il flusso non puo' avanzare perche' il piu' lento e' lontano. Se dura da TS_BLOCK_MS e gli altri in linea
// sono avanti (di TS_SLOW_AHEAD blocchi o piu') e aspettano dati (finestra quasi vuota), il piu' lento viene messo in recupero: gli altri
// ripartono, lui si recupera a parte con la sua quota (o per intero quando gli altri hanno finito).
static void check_blocked(ts_flow* f, uint32_t now) {
    uint32_t minc = flow_min_cursor(f);
    int blocked = f->next < f->total && (uint64_t) f->next + f->safety >= (uint64_t) minc + f->W;
    if(!blocked) {
        f->blockedSince = 0;
        return;
    }

    if(f->blockedSince == 0) {
        f->blockedSince = now ? now : 1;
        return;
    }

    if(age_of(now, f->blockedSince) < TS_BLOCK_MS) {
        return;
    }

    int holder = -1;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        if(in_set(&f->cl[i]) && !f->cl[i].done && f->cl[i].cursor == minc) {
            holder = i;
            break;
        }
    }

    if(holder < 0) {
        return;
    }

    int others = 0, starving = 0;
    for(int j = 0; j < TS_MAX_CLIENTS; j++) {
        const ts_client* c = &f->cl[j];
        if(j == holder || !in_set(c) || c->done) {
            continue;
        }

        others++;
        if(c->cursor > (uint64_t) minc + TS_SLOW_AHEAD && c->hi <= c->cursor + 512 && age_of(now, c->lastHeard) < 1500) {
            starving++;
        }
    }

    if(others > 0 && starving == others) {
        ts_client* h = &f->cl[holder];
        h->behind = 1;
        h->nCatch = 0;
        h->catchPos = 0;
        h->catchAt = 0;
        h->catchHi = h->hi > h->cursor ? h->hi : h->cursor;
        drop_requests(f, holder);
        f->blockedSince = 0;
        advance_base(f, flow_min_cursor(f));
    }
}

void flow_ack(ts_flow* f, uint16_t node, uint32_t now, uint32_t cursor, const ts_range* r, int n, int flags) {
    int done = (flags & TS_F_DONE) != 0;
    uint32_t tailHi = 0, tailFree = 0;
    int hasTail = 0;
    if((flags & TS_F_TAIL) && n > 0) {
        // l'ultimo intervallo e' il segnaposto: dice dove finisce quello che il ricevente ha visto e quanto spazio ha ancora, non e' un buco
        tailHi = r[n - 1].start;
        tailFree = r[n - 1].count;
        hasTail = 1;
        n--;
    }

    int ci = flow_client(f, node, now);
    if(ci < 0) {
        return;
    }

    ts_client* c = &f->cl[ci];
    c->lagging = 0;
    if(cursor > f->total) {
        cursor = f->total; // un cursore oltre la fine non esiste (frame sbagliato o contraffatto)
    }

    if(hasTail) {
        c->hi = tailHi; // dove finisce quello che ha visto: serve a sapere se ha la finestra piena o vuota
    } else {
        c->hi = cursor;
    }

    if(cursor > c->cursor) {
        c->cursor = cursor;
    }

    if(done) {
        c->done = 1;
    }

    if(c->late) {
        // Chi entra a giro in corso si aggancia solo se il giro ha i suoi titoli, la sua finestra basta e il suo cursore e' gia'
        // alla base (cioe' i blocchi che gli servono devono ancora passare). Altrimenti aspetta: nessuna richiesta, nessun recupero.
        int act = 0;
        for(int j = 0; j < TS_MAX_CLIENTS; j++) {
            if(j != ci && f->cl[j].used && !f->cl[j].done && !f->cl[j].late) {
                act++;
            }
        }

        if(act > 0 && c->mayRide && eff_cursor(c) >= f->base && cap_of(c) >= f->W) {
            c->late = 0;
            c->everIn = 1;
        } else {
            return;
        }
    }

    // Rientro di chi era rimasto indietro: se gli altri sono avanti non si riporta la finestra indietro, lo si recupera a parte.
    int others = 0;
    for(int j = 0; j < TS_MAX_CLIENTS; j++) {
        if(j != ci && in_set(&f->cl[j])) {
            others++;
        }
    }

    if(c->behind) {
        if(c->cursor >= f->base || others == 0) {
            c->behind = 0; // ha raggiunto gli altri, oppure gli altri non ci sono piu': e' lui la finestra
            c->nCatch = 0;
        }
    } else if(c->cursor < f->base) {
        if(others > 0) {
            c->behind = 1;
            c->nCatch = 0;
            c->catchPos = 0;
            c->catchAt = 0;
            drop_requests(f, ci);
        }
    }

    uint32_t minc = flow_min_cursor(f);
    if(minc < f->base) {
        // l'unico ricevente della finestra e' rientrato piu' indietro (dopo una pausa): si riparte dal suo cursore
        reset_window(f, minc);
    } else {
        advance_base(f, minc);
    }

    // Tutti quelli in linea sono gia' oltre il prossimo blocco nuovo (per esempio chi vuole solo un titolo piu' avanti, saltando gli altri):
    // nessuno ha bisogno di cio' che sta in mezzo, si salta avanti.
    if(flow_inline_count(f) > 0 && minc > f->next && minc <= f->total) {
        f->next = minc;
    }

    if(c->behind) {
        int k = 0;
        for(int i = 0; i < n && k < TS_MAX_RANGES; i++) {
            if(r[i].count == 0) {
                continue;
            }

            c->catchR[k++] = r[i];
        }

        c->nCatch = k;
        c->catchHi = hasTail ? tailHi : c->cursor;
        // un blocco inviato da meno di 300 ms e' ancora in volo: si prosegue da dove si era, altrimenti si riparte dal cursore
        c->catchPos = (c->catchAt != 0 && age_of(now, c->catchAt - 1) < 300) ? c->catchLast + 1 : 0;
        return;
    }

    // Le richieste di questo ricevente si ricostruiscono a ogni riscontro: ci sono tutti i suoi buchi aggiornati.
    drop_requests(f, ci);
    for(int i = 0; i < n; i++) {
        // gli intervalli che iniziano dopo l'ultimo blocco visto (la "coda persa") non sono perdite: il ricevente non li ha mai visti
        add_req_range(f, ci, r[i].start, (uint64_t) r[i].start + r[i].count, !(hasTail && r[i].start >= tailHi), now);
    }

    // Coda: il ricevente non ha mai visto i blocchi fra `tailHi` e il mittente (e' stato lontano, o e' rimasto solo dopo che gli altri hanno finito).
    // Se il divario e' grande li si richiede tutti, fino a quanto la sua finestra puo' tenere, senza aspettare che lo chieda a pezzi: altrimenti
    // il recupero procede a scatti. Il margine TS_TAIL_GAP lascia fuori i frame ancora in volo.
    if(hasTail && tailFree > 0 && (uint64_t) tailHi + TS_TAIL_GAP < f->next) {
        uint64_t to = (uint64_t) f->next - TS_TAIL_GAP;
        if(to > (uint64_t) tailHi + tailFree) {
            to = (uint64_t) tailHi + tailFree;
        }

        add_req_range(f, ci, tailHi, to, 0, now);
    }
}

void flow_expire(ts_flow* f, uint32_t now, uint32_t timeoutMs) {
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        ts_client* c = &f->cl[i];
        if(c->used && age_of(now, c->lastHeard) > timeoutMs) {
            drop_requests(f, i);
            c->used = 0;
            c->lost = !c->done; // chi aveva finito non conta come perso
            c->lagging = 0;
            c->behind = 0;
            c->nCatch = 0;
        }
    }

    // Chi tace da TS_LAG_MS mentre qualcun altro si fa sentire esce dalla finestra: gli altri non aspettano.
    int anyFresh = 0;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        if(f->cl[i].used && age_of(now, f->cl[i].lastHeard) <= TS_LAG_MS) {
            anyFresh = 1;
        }
    }

    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        ts_client* c = &f->cl[i];
        if(!c->used) {
            continue;
        }

        int lag = anyFresh && age_of(now, c->lastHeard) > TS_LAG_MS;
        if(lag && !c->lagging) {
            drop_requests(f, i);
        }

        c->lagging = lag;
    }

    if(flow_active(f) > 0) {
        advance_base(f, flow_min_cursor(f));
        check_blocked(f, now);
    }
}

int flow_merge_lap(ts_flow* f, uint32_t* pos, uint32_t* w) {
    if(flow_inline_count(f) == 0 || flow_late_count(f) == 0) {
        return 0;
    }

    uint32_t m = UINT32_MAX, ww = f->W;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        const ts_client* c = &f->cl[i];
        if(c->used && !c->done && c->late && !c->noMerge) {
            if(eff_cursor(c) < m) {
                m = eff_cursor(c);
            }

            if(cap_of(c) < ww) {
                ww = cap_of(c);
            }
        }
    }

    if(m == UINT32_MAX || m >= f->next || f->next >= f->total) {
        return 0; // niente da rifare: i nuovi sono avanti (potrebbero agganciarsi) oppure il giro e' finito
    }

    uint32_t cost = f->next - m, remaining = f->total - f->next;
    if((uint64_t) cost > 2ULL * remaining) {
        return 0; // si e' gia' troppo avanti: conviene che i nuovi aspettino il giro dopo
    }

    if(ww < 2048) {
        ww = 2048;
    }

    if(ww > f->Wcap) {
        ww = f->Wcap;
    }

    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        ts_client* c = &f->cl[i];
        if(c->used && !c->done && c->late && !c->noMerge) {
            c->late = 0;
            c->everIn = 1;
            c->behind = 0;
            c->lagging = 0;
            c->nCatch = 0;
        }
    }

    memset(f->pend, 0, f->Wcap);
    memset(f->pc, 0, (size_t) TS_MAX_CLIENTS * f->Wcap);
    memset(f->sentAt, 0, sizeof(uint32_t) * f->Wcap);
    memset(f->reqSeen, 0, f->Wcap);
    f->pendCount = 0;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        f->cl[i].nReq = 0;
        f->cl[i].scanPos = 0;
    }

    f->blockedSince = 0;
    f->W = ww;
    f->next = m;
    f->base = m;
    f->scan = m;
    f->lap++;
    f->lapStart = m;
    *pos = m;
    *w = ww;
    return 1;
}

void flow_demote(ts_flow* f, int idx) {
    if(idx < 0 || idx >= TS_MAX_CLIENTS || !f->cl[idx].used) {
        return;
    }

    drop_requests(f, idx);
    f->cl[idx].late = 1;
    f->cl[idx].behind = 0;
    f->cl[idx].lagging = 0;
    f->cl[idx].nCatch = 0;
}

void flow_drop(ts_flow* f, int idx) {
    if(idx < 0 || idx >= TS_MAX_CLIENTS || !f->cl[idx].used) {
        return;
    }

    drop_requests(f, idx);
    memset(&f->cl[idx], 0, sizeof(f->cl[idx]));
}

int flow_update_lap(ts_flow* f, uint32_t* pos, uint32_t* w) {
    int inl = flow_inline_count(f), lt = flow_late_count(f);
    if(inl != 0 || lt == 0) {
        return 0;
    }

    uint32_t m = UINT32_MAX, ww = f->Wcap;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        const ts_client* c = &f->cl[i];
        if(c->used && !c->done && c->late) {
            if(eff_cursor(c) < m) {
                m = eff_cursor(c);
            }

            if(cap_of(c) < ww) {
                ww = cap_of(c);
            }
        }
    }

    if(ww < 2048) {
        ww = 2048;
    }

    if(ww > f->Wcap) {
        ww = f->Wcap; // le tabelle sono dimensionate su Wcap
    }

    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        ts_client* c = &f->cl[i];
        if(c->used && !c->done && c->late) {
            c->late = 0;
            c->everIn = 1;
            c->behind = 0;
            c->lagging = 0;
            c->nCatch = 0;
        }
    }

    // nuovo giro: tutte le tabelle ripartono (la finestra puo' cambiare, quindi si azzerano per intero)
    memset(f->pend, 0, f->Wcap);
    memset(f->pc, 0, (size_t) TS_MAX_CLIENTS * f->Wcap);
    memset(f->sentAt, 0, sizeof(uint32_t) * f->Wcap);
    memset(f->reqSeen, 0, f->Wcap);
    f->pendCount = 0;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        f->cl[i].nReq = 0;
    }

    f->blockedSince = 0;
    f->W = ww;
    f->next = m;
    f->base = m;
    f->scan = m;
    f->lap++;
    f->lapStart = m;
    *pos = m;
    *w = ww;
    return 1;
}

// Quanti riceventi in linea (con ancora da ricevere) aspettano il blocco g.
static int fresh_k(const ts_flow* f, uint32_t g) {
    int k = 0;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        const ts_client* c = &f->cl[i];
        if(in_set(c) && !c->done && c->cursor <= g && (c->ncskip == 0 || ts_skip_adjust(c->cskip, c->ncskip, g) == g)) {
            k++;
        }
    }

    return k;
}

// Primo blocco di recupero da inviare (dal primo ricevente "behind" che ha qualcosa da ricevere): si procede in sequenza
// dal cursore; dove il ricevente ha gia' i blocchi (sotto catchHi e fuori dai suoi buchi) si salta; oltre catchHi si spinge
// fino al limite della sua finestra (cursore + W).
static int catch_next(const ts_flow* f, uint32_t* block, int* client) {
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        const ts_client* c = &f->cl[i];
        if(!(c->used && c->behind) || c->lagging) {
            continue; // chi e' in silenzio non riceve recuperi: non risponde, sarebbe aria sprecata
        }

        uint32_t limit = c->cursor + f->W - 16;
        if(limit > f->next) {
            limit = f->next;
        }

        uint32_t g = c->catchPos > c->cursor ? c->catchPos : c->cursor;
        for(int guard = 0; guard < 2 * TS_MAX_RANGES + 4; guard++) {
            g = ts_skip_adjust(f->skip, f->nSkip, g);
            if(g >= limit) {
                break;
            }

            if(g >= c->catchHi) {
                *block = g;
                *client = i;
                return 1;
            }

            uint32_t nextStart = c->catchHi;
            int inHole = 0;
            for(int k = 0; k < c->nCatch; k++) {
                uint32_t s = c->catchR[k].start, e = s + c->catchR[k].count;
                if(s <= g && g < e) {
                    inHole = 1;
                    break;
                }

                if(s > g && s < nextStart) {
                    nextStart = s;
                }
            }

            if(inHole) {
                *block = g;
                *client = i;
                return 1;
            }

            g = nextStart; // il ricevente ha questi blocchi: si salta al prossimo buco (o a catchHi)
        }
    }

    return 0;
}

int flow_is_pending(const ts_flow* f, uint32_t block) {
    return block >= f->base && block < f->base + f->W && block < f->next && f->pend[block % f->W] != 0;
}

int flow_pick(ts_flow* f, uint32_t* block, int* kind) {
    if(flow_inline_count(f) == 0) {
        return 0; // nessuno in linea: chi aspetta un giro nuovo non riceve nulla finche' non parte
    }

    // 1) ripetizione: per ogni ricevente si guarda la sua prima richiesta (indice piu' basso); si sceglie quella chiesta da piu' riceventi, a parita'
    // quella del ricevente con meno richieste in coda (chi e' quasi a posto si sblocca subito: un ricevente con migliaia di buchi non deve
    // far aspettare chi ne ha pochi), a parita' l'indice piu' basso.
    int haveRep = 0;
    uint32_t rep = 0;
    if(f->pendCount > 0) {
        uint32_t lim = f->base + f->W;
        if(lim > f->next) {
            lim = f->next;
        }

        uint32_t bestK = 0, bestN = 0;
        for(int i = 0; i < TS_MAX_CLIENTS; i++) {
            ts_client* c = &f->cl[i];
            if(!c->used || c->nReq == 0 || c->late || c->behind) {
                continue;
            }

            const uint8_t* row = &f->pc[(size_t) i * f->W];
            uint32_t pos = c->scanPos < f->base ? f->base : c->scanPos;
            uint32_t slot = pos % f->W;
            while(pos < lim && row[slot] == 0) {
                pos++;
                slot = slot + 1 == f->W ? 0 : slot + 1;
            }

            c->scanPos = pos;
            if(pos >= lim) {
                continue;
            }

            uint32_t k = f->pend[slot];
            if(!haveRep || k > bestK || (k == bestK && (c->nReq < bestN || (c->nReq == bestN && pos < rep)))) {
                haveRep = 1;
                rep = pos;
                bestK = k;
                bestN = c->nReq;
            }
        }
    }

    // 2) blocco nuovo
    uint32_t nx = ts_skip_adjust(f->skip, f->nSkip, f->next);
    int haveFresh = nx < f->total && nx + f->safety < flow_min_cursor(f) + f->W;

    // Molteplicita': una ripetizione chiesta da pochi vale meno di un blocco nuovo atteso da piu' riceventi in linea (ogni TS_MULT_TURN scelte
    // una ripetizione passa comunque: chi e' solo a chiederla non resta senza).
    if(haveRep && haveFresh) {
        uint32_t bestReq = f->pend[rep % f->W];
        int kf = fresh_k(f, nx);
        if((int) bestReq < kf) {
            if(++f->multTurn % TS_MULT_TURN != 0) {
                haveRep = 0; // si prende il blocco nuovo
            }
        }
    }

    // 3) recupero di chi e' rimasto indietro
    uint32_t cg = 0;
    int cc = -1;
    int haveCatch = catch_next(f, &cg, &cc);

    if(haveCatch) {
        int other = haveRep || haveFresh;
        int take = !other;
        if(other) {
            f->turn += TS_CATCH_SHARE_PCT;
            if(f->turn >= 100) {
                f->turn -= 100;
                take = 1;
            }
        }

        if(take) {
            *block = cg;
            *kind = TS_PICK_CATCH;
            f->pickClient = cc;
            return 1;
        }
    }

    if(haveRep) {
        *block = rep;
        *kind = TS_PICK_REPAIR;
        return 1;
    }

    if(haveFresh) {
        *block = nx;
        *kind = TS_PICK_FRESH;
        return 1;
    }

    return 0;
}

void flow_sent(ts_flow* f, uint32_t block, int kind, uint32_t now) {
    if(kind == TS_PICK_REPAIR) {
        uint32_t slot = block % f->W;
        req_clear_slot(f, slot);
        f->sentAt[slot] = now + 1;
        f->reqSeen[slot] = 0; // se va perso di nuovo e' un'altra perdita
        f->statRepairs++;
    } else if(kind == TS_PICK_CATCH) {
        f->statCatch++;
        if(f->pickClient >= 0 && f->pickClient < TS_MAX_CLIENTS) {
            ts_client* c = &f->cl[f->pickClient];
            c->catchPos = block + 1;
            c->catchLast = block;
            c->catchAt = now + 1;
        }
    } else if(block >= f->next) {
        f->next = block + 1;
        f->statFresh++;
    }
}

int flow_all_done(const ts_flow* f) {
    int any = 0;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        if(f->cl[i].used) {
            any = 1;
            if(!f->cl[i].done) {
                return 0;
            }
        }
    }

    return any;
}

void flow_set_skips(ts_flow* f, const ts_range* r, int n) {
    if(n > TS_MAX_TITLES + 1) {
        n = TS_MAX_TITLES + 1;
    }

    memcpy(f->skip, r, sizeof(ts_range) * (size_t) n);
    f->nSkip = n;
}
