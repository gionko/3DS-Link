#include "sched.h"

#include <stdlib.h>
#include <string.h>

#define NONE 0xFFFFFFFFu

static int ge(uint32_t a, uint32_t b) {
    return (int32_t) (a - b) >= 0;
}

static int alive(const ts_sched* S, const ts_sclient* c, uint32_t now) {
    (void) S;
    if(!c->used) {
        return 0;
    }

    if(!ge(now, c->lastHeard) || now - c->lastHeard > TS_S_LAG_MS) {
        return 0;
    }

    return c->acked || now - c->enteredAt < TS_S_UNACKED_MS;
}

// Efficienza usata per decidere urgenza e turno: dopo un'interruzione della linea e' limitata, anche se subito dopo il ricevente riceve bene.
static uint32_t eff_of(const ts_sclient* c, uint32_t now) {
    if(c->flakyUntil != 0 && !ge(now, c->flakyUntil)) {
        return c->eff < 400 ? c->eff : 400;
    }

    return c->eff;
}

static int fresh(const ts_sclient* c, uint32_t now) {
    return ge(now, c->lastHeard) && now - c->lastHeard < TS_FRESH_MS;
}

static int needs(const ts_sclient* c, int t) {
    return ts_bit(c->want, t) && c->remain[t] > 0 && (c->flags[t] & TS_SF_ACCEPT) && !(c->flags[t] & (TS_SF_DONE | TS_SF_ERROR));
}

static int in_ranges(const ts_range* r, int n, uint32_t b) {
    int lo = 0, hi = n - 1, f = -1;
    while(lo <= hi) {
        int mid = (lo + hi) / 2;
        if(r[mid].start <= b) {
            f = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    return f >= 0 && b - r[f].start < r[f].count;
}

// Primo blocco locale del titolo t che il ricevente non puo' ricevere ora: se sta installando il titolo, oltre la sua finestra (inst + win) i blocchi
// non entrerebbero (il ricevente non parcheggia davanti all'installazione). Ritorna 0xFFFFFFFF se non c'e' limite.
static uint32_t win_limit(const ts_sclient* c, int t) {
    if(c->win > 0 && (c->flags[t] & TS_SF_INSTALLING) && !(c->flags[t] & TS_SF_PARKOK)) {
        return c->inst[t] + c->win;
    }

    return 0xFFFFFFFFu;
}

// Il ricevente sta installando un ALTRO titolo di cui deve ancora ricevere dei blocchi? Allora il titolo t e' per dopo: parcheggiarlo adesso
// scrive sulla SD su cui AM sta installando e la rallenta (prova su console 12.10: AM da 2 a 0,5 MiB/s), quindi non lo chiede.
static int busy_elsewhere(const ts_sclient* c, int t) {
    if(c->remain[t] <= TS_TINY_BLOCKS) {
        return 0; // un rimasuglio (pochi blocchi per chiudere il titolo) si chiede sempre: parcheggiarlo costa pochi MiB (prova C1: 3 blocchi mancanti di un DLC hanno atteso la fine del titolo da 482 MiB)
    }

    for(int u = 0; u < TS_MAX_TITLES; u++) {
        if(u != t && (c->flags[u] & TS_SF_INSTALLING) && c->remain[u] > 0 && ts_bit(c->want, u)) {
            return 1;
        }
    }

    return 0;
}

// Ha qualcosa da ricevere adesso (il primo blocco che manca e' dentro la finestra, se sta installando il titolo; nessun altro titolo in installazione
// che aspetta ancora dati)?
static int useful(const ts_sclient* c, int t) {
    return needs(c, t) && c->first[t] < win_limit(c, t) && !busy_elsewhere(c, t);
}

// 1 se al ricevente manca ancora il blocco locale b del titolo t (secondo l'ultimo riscontro) e puo' riceverlo.
static int is_missing(const ts_sclient* c, int t, uint32_t b) {
    if(!needs(c, t) || b >= win_limit(c, t) || busy_elsewhere(c, t)) {
        return 0;
    }

    if(c->focus == t && c->nRanges > 0) {
        return in_ranges(c->r, c->nRanges, b);
    }

    return b >= c->first[t];
}

static uint32_t hslot(uint32_t g) {
    return (g * 2654435761u) >> 18; // 14 bit
}

static int recent(const ts_sched* S, uint32_t g, uint32_t now) {
    uint32_t h = hslot(g) & (TS_S_RECENT_SLOTS - 1);
    return S->recentG[h] == g + 1 && ge(now, S->recentMs[h]) && now - S->recentMs[h] < TS_S_INFLIGHT_MS;
}

static void mark_recent(ts_sched* S, uint32_t g, uint32_t now) {
    uint32_t h = hslot(g) & (TS_S_RECENT_SLOTS - 1);
    S->recentG[h] = g + 1;
    S->recentMs[h] = now;
}

ts_sched* sched_create(const ts_manifest* m) {
    ts_sched* S = (ts_sched*) calloc(1, sizeof(ts_sched));
    if(S == NULL) {
        return NULL;
    }

    S->recentG = (uint32_t*) calloc(TS_S_RECENT_SLOTS, sizeof(uint32_t));
    S->recentMs = (uint32_t*) calloc(TS_S_RECENT_SLOTS, sizeof(uint32_t));
    if(S->recentG == NULL || S->recentMs == NULL) {
        sched_destroy(S);
        return NULL;
    }

    S->m = m;
    S->cur = -1;
    S->betterTitle = -1;
    S->dirty = 1;
    return S;
}

void sched_destroy(ts_sched* S) {
    if(S == NULL) {
        return;
    }

    free(S->recentG);
    free(S->recentMs);
    free(S);
}

void sched_set_window(ts_sched* S, uint16_t token, uint32_t blocks) {
    int i = sched_find(S, token);
    if(i >= 0 && S->cl[i].win != blocks) {
        S->cl[i].win = blocks;
        S->dirty = 1;
    }
}

int sched_find(const ts_sched* S, uint16_t token) {
    for(int i = 0; i < TS_S_MAX; i++) {
        if(S->cl[i].used && S->cl[i].token == token) {
            return i;
        }
    }

    return -1;
}

static void clear_slot(ts_sched* S, int i) {
    memset(&S->cl[i], 0, sizeof(S->cl[i]));
    S->dirty = 1;
}

static void init_needs(ts_sched* S, ts_sclient* c, const uint8_t* bits) {
    const ts_manifest* m = S->m;
    memcpy(c->want, bits, TS_BITMAP_BYTES);
    for(int t = 0; t < m->count; t++) {
        if(ts_bit(bits, t)) {
            c->remain[t] = m->t[t].blocks;
            c->first[t] = 0;
            c->flags[t] = TS_SF_ACCEPT;
        } else {
            c->remain[t] = 0;
            c->flags[t] = 0;
        }
    }
}

int sched_ready(ts_sched* S, uint16_t token, uint16_t ident, const uint8_t* bits, uint32_t freeMiB, uint32_t now, uint16_t* replaced) {
    if(replaced != NULL) {
        *replaced = 0;
    }

    if(token == 0 || S->m->count < 1 || S->m->count > TS_MAX_TITLES) {
        return -1;
    }

    for(int k = 0; k < 4; k++) {
        if(S->byeTok[k] == token && ge(now, S->byeAt[k]) && now - S->byeAt[k] < 3000) {
            return -2;
        }
    }

    if(ident != 0) {
        for(int j = 0; j < TS_S_MAX; j++) {
            ts_sclient* o = &S->cl[j];
            if(o->used && o->ident == ident && o->token != token && ge(now, o->lastHeard) && now - o->lastHeard > 2000) {
                if(replaced != NULL) {
                    *replaced = o->token;
                }

                clear_slot(S, j);
            }
        }
    }

    int idx = sched_find(S, token);
    int isNew = idx < 0;
    if(isNew) {
        for(int i = 0; i < TS_S_MAX; i++) {
            if(!S->cl[i].used) {
                idx = i;
                break;
            }
        }

        if(idx < 0) {
            return -1;
        }

        uint32_t minServed = NONE;
        for(int i = 0; i < TS_S_MAX; i++) {
            if(S->cl[i].used && S->cl[i].served < minServed) {
                minServed = S->cl[i].served;
            }
        }

        ts_sclient* c = &S->cl[idx];
        memset(c, 0, sizeof(*c));
        c->used = 1;
        c->token = token;
        c->enteredAt = now ? now : 1;
        c->focus = TS_NO_TITLE;
        c->hint = NONE;
        c->hintTitle = TS_NO_TITLE;
        c->served = minServed == NONE ? 0 : minServed; // chi entra non deve recuperare tutto il passato prima di avere il suo turno
        c->freeMiB = NONE;
        c->eff = 1000;
        c->effT = TS_NO_TITLE;
        init_needs(S, c, bits);
        S->dirty = 1;
    }

    ts_sclient* c = &S->cl[idx];
    c->ident = ident;
    if(!c->acked) {
        c->lastHeard = now; // chi ha cominciato a rispondere lo si giudica dai riscontri
        if(!isNew && memcmp(c->want, bits, TS_BITMAP_BYTES) != 0) {
            init_needs(S, c, bits);
            S->dirty = 1;
        }

        c->freeMiB = freeMiB;
    }

    return isNew ? 1 : 0;
}

int sched_ack(ts_sched* S, uint16_t token, uint32_t now, const ts_ack2* a) {
    int idx = sched_find(S, token);
    if(idx < 0) {
        return 0;
    }

    const ts_manifest* m = S->m;
    ts_sclient* c = &S->cl[idx];
    if(c->acked && ge(now, c->lastHeard) && now - c->lastHeard >= TS_FLAKY_GAP_MS) {
        c->flakyUntil = now + TS_FLAKY_MS; // i riscontri erano spariti e sono tornati: linea instabile
    }

    int truncated = a->nSum >= TS_ACK2_MAX_SUM;
    uint8_t seen[TS_MAX_TITLES];
    memset(seen, 0, sizeof(seen));
    for(int i = 0; i < a->nSum; i++) {
        int t = a->sum[i].title;
        if(t >= m->count || !ts_bit(c->want, t)) {
            continue;
        }

        seen[t] = 1;
        c->flags[t] = a->sum[i].flags;
        c->remain[t] = (a->sum[i].flags & (TS_SF_DONE | TS_SF_ERROR)) ? 0 : a->sum[i].remain;
        c->first[t] = a->sum[i].first;
        c->inst[t] = a->sum[i].inst;
    }

    for(int t = 0; t < m->count && !truncated; t++) {
        if(ts_bit(c->want, t) && !seen[t]) {
            c->remain[t] = 0; // non e' nel riepilogo dei titoli non finiti: finito
            c->flags[t] = TS_SF_DONE;
        }
    }

    c->focus = TS_NO_TITLE;
    c->nRanges = 0;
    if(a->focus < m->count) {
        uint32_t n = m->t[a->focus].blocks, prevEnd = 0;
        int k = 0;
        for(int i = 0; i < a->nRanges && k < TS_ACK2_MAX_RANGES; i++) {
            uint32_t s = a->r[i].start;
            uint64_t e = (uint64_t) s + a->r[i].count;
            if(e > n) {
                e = n;
            }

            if(s >= n || e <= s || s < prevEnd) {
                continue; // fuori dal titolo, vuoto o sovrapposto al precedente: non e' un intervallo valido
            }

            c->r[k].start = s;
            c->r[k].count = (uint32_t) (e - s);
            prevEnd = (uint32_t) e;
            k++;
        }

        c->focus = a->focus;
        c->nRanges = k;
    }

    if(S->cur >= 0) {
        int t = S->cur;
        if(c->effT != t) {
            c->effT = (uint8_t) t;
            c->effRemain = c->remain[t];
            c->effNeed = c->needSent;
            c->effAt = now;
        } else if(ge(now, c->effAt + 1000)) {
            uint32_t dS = c->needSent - c->effNeed;
            if(dS >= 40) {
                uint32_t dR = c->effRemain > c->remain[t] ? c->effRemain - c->remain[t] : 0;
                uint64_t e = (uint64_t) dR * 1000 / dS;
                c->eff = e > 1000 ? 1000 : (uint32_t) e;
                c->effRemain = c->remain[t];
                c->effNeed = c->needSent;
                c->effAt = now;
            } else if(ge(now, c->effAt + 6000)) {
                c->effRemain = c->remain[t]; // troppo poco servito per giudicare: si riparte
                c->effNeed = c->needSent;
                c->effAt = now;
            }
        }
    }

    c->freeMiB = a->freeMiB;
    if(c->instSampleMs == 0 || a->installed < c->instSample) {
        c->instSampleMs = now;
        c->instSample = a->installed;
    } else if(ge(now, c->instSampleMs + 2000)) {
        double dt = (double) (now - c->instSampleMs) / 1000.0;
        double r = (double) (a->installed - c->instSample) / dt;
        c->instRate = c->instRate <= 0.0 ? r : 0.9 * c->instRate + 0.1 * r; // ~20 s di memoria: l'ETA non deve ballare
        c->instSampleMs = now;
        c->instSample = a->installed;
    }

    c->installed = a->installed;
    c->acked = 1;
    c->lastHeard = now;
    c->retryAt = 0;
    S->dirty = 1;
    return 1;
}

int sched_bye(ts_sched* S, uint16_t token, uint32_t now) {
    int idx = sched_find(S, token);
    S->byeIdent[S->byeNext & 3] = idx >= 0 ? S->cl[idx].ident : 0;
    S->byeTok[S->byeNext & 3] = token;
    S->byeAt[S->byeNext & 3] = now;
    S->byeNext++;
    if(idx < 0) {
        return 0;
    }

    clear_slot(S, idx);
    return 1;
}

int sched_expire(ts_sched* S, uint32_t now, uint16_t* dropped, int max) {
    int n = 0;
    for(int i = 0; i < TS_S_MAX; i++) {
        ts_sclient* c = &S->cl[i];
        if(c->used && ge(now, c->lastHeard) && now - c->lastHeard >= TS_S_LOST_MS) {
            if(dropped != NULL && n < max) {
                dropped[n] = c->token;
            }

            n++;
            clear_slot(S, i);
        }
    }

    return n;
}

static void decide(ts_sched* S, uint32_t now) {
    const ts_manifest* m = S->m;
    int n = m->count;
    uint8_t k[TS_MAX_TITLES];
    uint32_t mx[TS_MAX_TITLES];
    uint32_t mnh[TS_MAX_TITLES];  // il minimo dei blocchi mancanti fra i riceventi che ricevono bene (efficienza sufficiente) e hanno ancora bisogno del titolo
    memset(k, 0, sizeof(k));
    memset(mx, 0, sizeof(mx));
    for(int t = 0; t < TS_MAX_TITLES; t++) {
        mnh[t] = 0xFFFFFFFFu;
    }
    for(int i = 0; i < TS_S_MAX; i++) {
        const ts_sclient* c = &S->cl[i];
        if(!alive(S, c, now)) {
            continue;
        }

        for(int t = 0; t < n; t++) {
            if(useful(c, t)) {
                k[t]++;
                if(c->remain[t] > mx[t]) {
                    mx[t] = c->remain[t];
                }

                if(eff_of(c, now) >= TS_EFF_MIN && c->remain[t] < mnh[t]) {
                    mnh[t] = c->remain[t];
                }
            }
        }
    }

    int tiny = -1, best = -1;
    for(int t = 0; t < n; t++) {
        if(k[t] == 0) {
            continue;
        }

        // rimasuglio: UN ricevente (che riceve bene: chi e' fermo o riceve male non tiene il mittente su un titolo) ha da ricevere poco per chiuderlo
        if(mnh[t] <= TS_TINY_BLOCKS && (tiny < 0 || mnh[t] < mnh[tiny])) {
            tiny = t;
        }

        if(best < 0 || k[t] > k[best] || (k[t] == k[best] && mx[t] < mx[best])) {
            best = t;
        }
    }

    int cur = S->cur, next;
    if(cur >= 0 && k[cur] > 0) {
        next = cur;
        if(tiny >= 0 && tiny != cur && mnh[cur] > TS_TINY_BLOCKS) {
            next = tiny; // un rimasuglio passa avanti subito
            S->st.tinySwitches++;
        } else if(best != cur && k[best] > k[cur]) {
            if(S->betterTitle == best) {
                if(ge(now, S->betterSince + TS_DWELL_MS)) {
                    next = best;
                    S->st.dwellSwitches++;
                }
            } else {
                S->betterTitle = best;
                S->betterSince = now;
            }
        } else {
            S->betterTitle = -1;
        }
    } else {
        next = tiny >= 0 ? tiny : best;
    }

    if(next != cur) {
        S->cur = next;
        S->gen++;
        S->betterTitle = -1;
        S->st.switches++;
        for(int i = 0; i < TS_S_MAX; i++) {
            S->cl[i].retryAt = 0;
        }
    }
}

// Primo blocco locale che serve al ricevente e non e' stato inviato da poco. 1 se c'e'.
static int find_cand(const ts_sched* S, const ts_sclient* c, int t, uint32_t now, uint32_t* out) {
    const ts_title* T = &S->m->t[t];
    uint32_t n = T->blocks;
    ts_range one;
    const ts_range* rs;
    int nr;
    if(c->focus == t && c->nRanges > 0) {
        rs = c->r;
        nr = c->nRanges;
    } else {
        if(c->first[t] >= n) {
            return 0;
        }

        one.start = c->first[t];
        one.count = n - one.start;
        rs = &one;
        nr = 1;
    }

    uint32_t hint = c->hintTitle == t ? c->hint : NONE;
    uint32_t lim = win_limit(c, t);
    for(int j = 0; j < nr; j++) {
        uint32_t s = rs[j].start;
        uint64_t e64 = (uint64_t) s + rs[j].count;
        uint32_t e = e64 > n ? n : (uint32_t) e64;
        if(e > lim) {
            e = lim;
        }

        if(s >= e) {
            continue; // tutto il tratto e' oltre la finestra
        }

        if(hint != NONE && hint >= s && hint < e) {
            for(uint32_t b = hint; b < e; b++) {
                if(!recent(S, T->firstBlock + b, now)) {
                    *out = b;
                    return 1;
                }
            }

            for(uint32_t b = s; b < hint; b++) {
                if(!recent(S, T->firstBlock + b, now)) {
                    *out = b;
                    return 1;
                }
            }
        } else {
            for(uint32_t b = s; b < e; b++) {
                if(!recent(S, T->firstBlock + b, now)) {
                    *out = b;
                    return 1;
                }
            }
        }
    }

    return 0;
}

int sched_next(ts_sched* S, uint32_t now, uint32_t* g) {
    if(S->dirty || ge(now, S->decideAt)) {
        decide(S, now);
        S->dirty = 0;
        S->decideAt = now + TS_S_DECIDE_MS;
    }

    int t = S->cur;
    if(t < 0) {
        S->st.idle++;
        return 0;
    }

    uint32_t cand[TS_S_MAX];
    int has[TS_S_MAX];
    int nc = 0;
    for(int i = 0; i < TS_S_MAX; i++) {
        ts_sclient* c = &S->cl[i];
        has[i] = 0;
        if(!alive(S, c, now) || !useful(c, t) || !ge(now, c->retryAt)) {
            continue;
        }

        if(ge(now, c->sweepAt)) {
            c->hint = NONE;
            c->sweepAt = now + TS_S_INFLIGHT_MS;
        }

        if(find_cand(S, c, t, now, &cand[i])) {
            has[i] = 1;
            nc++;
        } else {
            c->retryAt = now + TS_S_RETRY_MS;
        }
    }

    if(nc == 0) {
        S->st.idle++;
        return 0;
    }

    int win = -1, turn = 0, urgent = 0;
    // un installatore a secco, il cui blocco e' lontano dalla marcia (chi e' entrato dopo, chi ha perso molto): va avanti a tutti, ma mai
    // piu' di TS_URGENT_RUN volte di fila e solo se riceve bene (altrimenti si spreca la radio)
    {
        uint32_t bestLead = 0;
        for(int i = 0; i < TS_S_MAX; i++) {
            const ts_sclient* c = &S->cl[i];
            // la quota di urgenza scende con la perdita: piena sopra il 90% di efficienza, meta' sopra il 70%, nessuna sotto
            uint32_t ef = eff_of(c, now);
            int run = ef >= 900 ? TS_URGENT_RUN : (ef >= 700 ? 1 : 0);
            if(!has[i] || !fresh(c, now) || !(c->flags[t] & TS_SF_INSTALLING) || S->urgentRun >= run || (uint64_t) cand[i] + TS_URGENT_BEHIND >= S->fr[t]) {
                continue;
            }

            uint32_t lead = c->first[t] > c->inst[t] ? c->first[t] - c->inst[t] : 0;
            // P1 ("il piu' indietro per primo", approvato dall'utente il 14.10): fra i riceventi urgenti vince il blocco da inviare piu' BASSO (il piu' indietro); a parita'
            // di blocco il lead minore. Il piu' indietro avanza finche' raggiunge l'altro ritardatario: da li' in poi lo stesso frame serve entrambi (molteplicita' 2 invece di 1).
            // Le condizioni per essere urgenti sono invariate. Simulazione: -4% di frame nelle repliche di difficolta-3/4, nessuna regressione dimostrata (RAPPORTO-SIMULAZIONE-P1.md).
            if(lead < TS_LEAD_LOW && (win < 0 || cand[i] < cand[win] || (cand[i] == cand[win] && lead < bestLead))) {
                win = i;
                bestLead = lead;
            }
        }

        if(win >= 0) {
            urgent = 1;
            S->urgentRun++;
            S->cl[win].urgServed++;
        }
    }

    if(win < 0) {
        S->urgentRun = 0;
        S->turn++;
        if(nc > 1 && S->turn % TS_S_TURN == 0) {
            // chi riceve bene ha il turno intero (1 su TS_S_TURN), chi riceve a meta' un turno su due, chi riceve male uno su quattro
            uint32_t ev = S->turn / TS_S_TURN;
            uint32_t minEff = ev % 4 == 0 ? 0 : (ev % 2 == 0 ? 700 : 900);
            for(int i = 0; i < TS_S_MAX; i++) {
                if(has[i] && fresh(&S->cl[i], now) && eff_of(&S->cl[i], now) >= minEff && (win < 0 || S->cl[i].served < S->cl[win].served)) {
                    win = i;
                }
            }

            turn = win >= 0;
        }

        if(win < 0) {
            int bestCount = 0;
            int cnts[TS_S_MAX] = {0};
            for(int i = 0; i < TS_S_MAX; i++) {
                if(!has[i]) {
                    continue;
                }

                int count = -1;
                for(int j = 0; j < i; j++) { // lo stesso blocco gia' contato per un altro ricevente: stesso risultato
                    if(has[j] && cand[j] == cand[i]) {
                        count = cnts[j];
                        break;
                    }
                }

                if(count < 0) {
                    count = 0;
                    for(int x = 0; x < TS_S_MAX; x++) {
                        if(alive(S, &S->cl[x], now) && is_missing(&S->cl[x], t, cand[i])) {
                            count++;
                        }
                    }
                }

                cnts[i] = count;

                if(win < 0 || count > bestCount || (count == bestCount && S->cl[i].remain[t] < S->cl[win].remain[t])) {
                    win = i;
                    bestCount = count;
                }
            }
        }
    }

    uint32_t b = cand[win];
    *g = S->m->t[t].firstBlock + b;
    mark_recent(S, *g, now);
    if(b + 1 > S->fr[t]) {
        S->fr[t] = b + 1;
    }

    for(int x = 0; x < TS_S_MAX; x++) {
        if(alive(S, &S->cl[x], now) && is_missing(&S->cl[x], t, b)) {
            S->cl[x].needSent++;
        }

        if(has[x] && cand[x] == b) {
            S->cl[x].served++;
            S->cl[x].hint = b + 1;
            S->cl[x].hintTitle = (uint8_t) t;
        }
    }

    S->st.picks++;
    if(turn) {
        S->st.turnPicks++;
    }

    if(urgent) {
        S->st.urgentPicks++;
    }

    return 1;
}

void sched_unsend(ts_sched* S, uint32_t g) {
    uint32_t h = hslot(g) & (TS_S_RECENT_SLOTS - 1);
    if(S->recentG[h] != g + 1) {
        return;
    }

    S->recentG[h] = 0;
    // il blocco non e' partito (coda del chip piena, lettura fallita): si annulla anche il conto fatto da sched_next, altrimenti l'efficienza
    // dei riceventi scende (con la radio satura meta' dei tentativi falliscono) e il suggerimento salta un blocco che nessuno ha ricevuto
    int t = S->cur;
    if(t < 0 || g < S->m->t[t].firstBlock || g >= S->m->t[t].firstBlock + S->m->t[t].blocks) {
        return;
    }

    uint32_t b = g - S->m->t[t].firstBlock;
    for(int i = 0; i < TS_S_MAX; i++) {
        ts_sclient* c = &S->cl[i];
        if(!c->used) {
            continue;
        }

        if(c->needSent > 0 && is_missing(c, t, b)) {
            c->needSent--;
        }

        if(c->hintTitle == t && c->hint == b + 1) {
            c->hint = b;
            if(c->served > 0) {
                c->served--;
            }
        }
    }
}

// Molteplicita' reale di un frame appena inviato: quanti riceventi vivi avevano ancora bisogno del blocco globale g (secondo l'ultimo riscontro). E' lo stesso
// controllo ("vivo" e is_missing) con cui sched_next conta needSent per ogni blocco scelto, ma SOLA LETTURA: non cambia nessuno stato dello schedulatore (solo
// strumentazione del registro del mittente). Chiamata subito dopo un invio riuscito, con lo stesso `now` della sched_next che ha scelto g, ritorna lo stesso
// numero che sched_next ha sommato ai needSent (i riscontri non sono cambiati nel frattempo). Costo O(TS_S_MAX) senza divisioni. 0 se g non e' di nessun titolo.
int sched_need_count(const ts_sched* S, uint32_t g, uint32_t now) {
    const ts_manifest* m = S->m;
    int t = S->cur;
    if(t < 0 || g < m->t[t].firstBlock || g - m->t[t].firstBlock >= m->t[t].blocks) {
        t = -1;
        for(int u = 0; u < m->count; u++) {
            if(g >= m->t[u].firstBlock && g - m->t[u].firstBlock < m->t[u].blocks) {
                t = u;
                break;
            }
        }

        if(t < 0) {
            return 0;
        }
    }

    uint32_t b = g - m->t[t].firstBlock;
    int n = 0;
    for(int x = 0; x < TS_S_MAX; x++) {
        if(alive(S, &S->cl[x], now) && is_missing(&S->cl[x], t, b)) {
            n++;
        }
    }

    return n;
}

int sched_cur(const ts_sched* S) {
    return S->cur;
}

int sched_serving(const ts_sched* S, uint32_t now) {
    int n = 0;
    for(int i = 0; i < TS_S_MAX; i++) {
        const ts_sclient* c = &S->cl[i];
        if(!alive(S, c, now)) {
            continue;
        }

        for(int t = 0; t < S->m->count; t++) {
            if(needs(c, t)) {
                n++;
                break;
            }
        }
    }

    return n;
}

int sched_alive(const ts_sched* S, uint32_t now) {
    int n = 0;
    for(int i = 0; i < TS_S_MAX; i++) {
        n += alive(S, &S->cl[i], now);
    }

    return n;
}

int sched_all_done(const ts_sched* S) {
    int any = 0;
    for(int i = 0; i < TS_S_MAX; i++) {
        const ts_sclient* c = &S->cl[i];
        if(!c->used) {
            continue;
        }

        any = 1;
        if(!c->acked) {
            return 0;
        }

        for(int t = 0; t < S->m->count; t++) {
            if(needs(c, t)) {
                return 0;
            }
        }
    }

    return any;
}

uint32_t sched_need_blocks(const ts_sched* S, const ts_sclient* c) {
    uint32_t n = 0;
    for(int t = 0; t < S->m->count; t++) {
        if(ts_bit(c->want, t)) {
            n += S->m->t[t].blocks;
        }
    }

    return n;
}

uint32_t sched_remain_blocks(const ts_sched* S, const ts_sclient* c) {
    uint32_t n = 0;
    for(int t = 0; t < S->m->count; t++) {
        if(ts_bit(c->want, t) && !(c->flags[t] & (TS_SF_DONE | TS_SF_ERROR))) {
            n += c->remain[t];
        }
    }

    return n;
}

int sched_finished(const ts_sched* S, const ts_sclient* c) {
    if(!c->acked) {
        return 0;
    }

    for(int t = 0; t < S->m->count; t++) {
        if(ts_bit(c->want, t) && !(c->flags[t] & (TS_SF_DONE | TS_SF_ERROR))) {
            return 0;
        }
    }

    return 1;
}

int sched_pct_down(const ts_sched* S, const ts_sclient* c) {
    uint32_t need = sched_need_blocks(S, c), rem = sched_remain_blocks(S, c);
    if(need == 0 || rem == 0) {
        return c->acked || need == 0 ? 100 : 0;
    }

    uint64_t have = need > rem ? need - rem : 0;
    int p = (int) (have * 100 / need);
    return p > 99 ? 99 : p;
}

int sched_pct_inst(const ts_sched* S, const ts_sclient* c) {
    uint32_t need = sched_need_blocks(S, c);
    if(sched_finished(S, c) || need == 0) {
        return 100;
    }

    uint64_t v = (uint64_t) c->installed * 100 / need;
    return v > 99 ? 99 : (int) v;
}

double sched_eta_sec(const ts_sched* S, const ts_sclient* c, uint32_t now) {
    if(sched_finished(S, c)) {
        return 0.0;
    }

    uint32_t need = sched_need_blocks(S, c);
    if(!c->acked || c->instRate < 5.0 || !ge(now, c->lastHeard) || now - c->lastHeard > TS_S_LAG_MS || c->installed >= need) {
        return -1.0;
    }

    return (double) (need - c->installed) / c->instRate;
}

void sched_stats(const ts_sched* S, ts_sched_stats* out) {
    *out = S->st;
}
