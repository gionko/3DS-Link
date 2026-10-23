#include "room.h"

#include <string.h>

int room_ride_ok(const ts_room* R, int idx) {
    if(!R->lapValid) {
        return 0;
    }

    int any = 0;
    for(int b = 0; b < TS_BITMAP_BYTES; b++) {
        if(R->want[idx][b] & ~R->lapUnion[b]) {
            return 0;
        }

        any |= R->want[idx][b];
    }

    return any != 0;
}

int room_ready(ts_room* R, const ts_manifest* m, uint16_t token, uint16_t ident, uint8_t flags, const uint8_t* bits, uint32_t now, uint16_t* replaced) {
    int count = m->count;
    if(replaced != NULL) {
        *replaced = 0;
    }

    if(token == 0 || count < 1 || count > TS_MAX_TITLES) {
        return -1;
    }

    for(int k = 0; k < 4; k++) {
        if(R->byeTok[k] == token && now >= R->byeAt[k] && now - R->byeAt[k] < 3000) {
            return -2; // READY partito prima dell'addio e arrivato dopo
        }
    }

    int restarted = 0;
    if(ident != 0) {
        for(int k = 0; k < 4; k++) {
            if(R->byeIdent[k] == ident && R->byeTok[k] != token && now >= R->byeAt[k] && now - R->byeAt[k] < 60000) {
                restarted = 1; // la stessa console che ha appena salutato rientra
            }
        }
    }

    int kn = flow_find(&R->f, token);
    int known = kn >= 0;
    uint32_t prevHeard = known ? R->f.cl[kn].lastHeard : 0;
    // a ogni READY (anche dopo l'avvio: il nuovo ricevente puo' cominciare a rispondere prima che la voce vecchia abbia taciuto 2 s) si controlla
    // se c'e' una voce vecchia della stessa console che ha taciuto abbastanza
    if(ident != 0) {
        // la stessa console con un token nuovo: e' stata riavviata, la voce vecchia non serve piu'. Solo se quella vecchia tace da piu' di 2 s:
        // due console con lo stesso identificativo (1 su 65536) vive entrambe non si cancellano a vicenda.
        for(int j = 0; j < TS_MAX_CLIENTS; j++) {
            if(R->f.cl[j].used && R->ident[j] == ident && R->tok[j] != token && now >= R->f.cl[j].lastHeard && now - R->f.cl[j].lastHeard > 2000) {
                if(replaced != NULL) {
                    *replaced = R->tok[j];
                }

                flow_drop(&R->f, j);
                R->ready[j] = 0;
                R->acked[j] = 0;
                R->tok[j] = 0;
                R->ident[j] = 0;
                R->inlineSince[j] = 0;
                restarted = 1;
            }
        }
    }

    int idx = flow_client(&R->f, token, now);
    if(idx < 0) {
        return -1;
    }

    if(known && R->acked[idx]) {
        R->f.cl[idx].lastHeard = prevHeard; // chi ha cominciato lo si giudica dai riscontri: un READY che arriva da solo non lo tiene in vita
    }

    if(R->tok[idx] != token) {
        R->tok[idx] = token; // slot nuovo (o riusato): si dimentica cio' che c'era
        R->ready[idx] = 0;
        R->acked[idx] = 0;
        R->cap[idx] = 0;
        memset(R->want[idx], 0, TS_BITMAP_BYTES);
    }

    R->ident[idx] = ident;
    int first = !R->ready[idx];
    // cio' che vuole e la finestra si accettano finche' non e' in linea: dopo non si cambiano
    if(first || R->f.cl[idx].late) {
        uint32_t cap = flags ? (uint32_t) flags * TS_WIN_UNIT : 4096;
        memcpy(R->want[idx], bits, TS_BITMAP_BYTES);
        R->cap[idx] = cap;
        R->ready[idx] = 1;
        flow_set_client_info(&R->f, idx, cap, room_ride_ok(R, idx));
        ts_range cs[TS_MAX_TITLES + 1];
        int nc = ts_skip_ranges(m, R->want[idx], cs, TS_MAX_TITLES + 1);
        flow_set_client_skips(&R->f, idx, cs, nc);
        uint32_t hint = 0;
        for(int t = 0; t < m->count; t++) {
            if(ts_bit(bits, t)) {
                hint = m->t[t].firstBlock; // il primo titolo voluto: da li' in poi gli serve
                break;
            }
        }

        flow_set_client_hint(&R->f, idx, hint);
    }

    if(restarted) {
        R->lastEntryAt = 0; // chi rientra dopo un riavvio e' lo stesso di prima: nessuna attesa di altri riceventi, il giro riparte subito
    } else if(first && !known) {
        R->lastEntryAt = now ? now : 1;
    }

    return (first && !known) ? 1 : 0;
}

int room_bye(ts_room* R, uint16_t token, uint32_t now) {
    {
        int bi = flow_find(&R->f, token);
        R->byeIdent[R->byeNext & 3] = bi >= 0 ? R->ident[bi] : 0;
    }

    R->byeTok[R->byeNext & 3] = token;
    R->byeAt[R->byeNext & 3] = now;
    R->byeNext++;
    int idx = flow_find(&R->f, token);
    if(idx < 0) {
        return 0;
    }

    flow_drop(&R->f, idx);
    R->ready[idx] = 0;
    R->acked[idx] = 0;
    R->tok[idx] = 0;
    R->ident[idx] = 0;
    R->inlineSince[idx] = 0;
    return 1;
}

int room_ack(ts_room* R, uint16_t token, uint32_t now, uint32_t cursor, const ts_range* r, int n, int flags) {
    int idx = flow_find(&R->f, token);
    if(idx < 0 || !R->ready[idx]) {
        return 0;
    }

    R->nAcks++;
    R->acked[idx] = 1;
    R->f.cl[idx].noMerge = 0;
    flow_ack(&R->f, token, now, cursor, r, n, flags);
    return 1;
}

int room_union(const ts_room* R, int count, uint8_t* bits, int onlyInline) {
    memset(bits, 0, TS_BITMAP_BYTES);
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        const ts_client* c = &R->f.cl[i];
        if(c->used && R->ready[i] && (!onlyInline || (!c->done && !c->late))) {
            for(int b = 0; b < TS_BITMAP_BYTES; b++) {
                bits[b] |= R->want[i][b];
            }
        }
    }

    int n = 0;
    for(int i = 0; i < count; i++) {
        n += ts_bit(bits, i);
    }

    return n;
}

int room_drop_unstarted(ts_room* R, uint32_t now, uint32_t timeoutMs, uint16_t* dropped, int max) {
    int n = 0;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        if(R->f.cl[i].used && !R->acked[i] && now >= R->f.cl[i].lastHeard && now - R->f.cl[i].lastHeard > timeoutMs) {
            if(n < max) {
                dropped[n] = R->f.cl[i].node;
            }

            n++;
            flow_drop(&R->f, i);
            R->ready[i] = 0;
            R->tok[i] = 0;
            R->ident[i] = 0;
        }
    }

    return n;
}

int room_drop_finished(ts_room* R, uint32_t now, uint32_t timeoutMs) {
    int n = 0;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        if(R->f.cl[i].used && R->f.cl[i].done && now >= R->f.cl[i].lastHeard && now - R->f.cl[i].lastHeard > timeoutMs) {
            flow_drop(&R->f, i);
            R->ready[i] = 0;
            R->acked[i] = 0;
            R->tok[i] = 0;
            R->ident[i] = 0;
            n++;
        }
    }

    return n;
}

int room_demote_silent(ts_room* R, uint32_t now, uint32_t timeoutMs, uint16_t* demoted, int max) {
    int n = 0;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        const ts_client* c = &R->f.cl[i];
        if(c->used && !c->done && !c->late && !R->acked[i] && R->inlineSince[i] != 0 && now >= R->inlineSince[i] && now - R->inlineSince[i] > timeoutMs) {
            if(n < max) {
                demoted[n] = c->node;
            }

            n++;
            flow_demote(&R->f, i);
            R->f.cl[i].noMerge = 1; // non risponde: finche' non dara' segno di vita non lo si rimette in linea con una fusione
            R->inlineSince[i] = 0;
        }
    }

    return n;
}

int room_update_lap(ts_room* R, const ts_manifest* m, uint32_t now, ts_range* skip, int maxSkip, int* nSkip, uint32_t* pos, uint32_t* w, int* nTitles) {
    if(flow_inline_count(&R->f) == 0 && flow_late_count(&R->f) > 0) {
        if(R->lateSince == 0) {
            R->lateSince = now ? now : 1;
        }

        uint32_t sinceEntry = now >= R->lastEntryAt ? now - R->lastEntryAt : 0;
        uint32_t sinceFirst = now >= R->lateSince ? now - R->lateSince : 0;
        if(R->lastEntryAt != 0 && sinceEntry < TS_LAP_QUIET_MS && sinceFirst < TS_LAP_MAX_WAIT_MS) {
            return 0; // qualcuno e' appena entrato: si aspetta che arrivi anche chi sta per confermare
        }
    } else if(!(flow_inline_count(&R->f) > 0 && flow_late_count(&R->f) > 0)) {
        R->lateSince = 0;
    }

    int fresh = 0;
    if(flow_inline_count(&R->f) > 0 && flow_late_count(&R->f) > 0) {
        // c'e' un giro in corso e qualcuno e' arrivato dopo: se conviene (si e' ancora nella prima parte) il flusso riparte per tutti, dopo un po'
        // di quiete (altri possono stare per entrare)
        if(R->lateSince == 0) {
            R->lateSince = now ? now : 1;
        }

        uint32_t sinceEntry = now >= R->lastEntryAt ? now - R->lastEntryAt : 0;
        uint32_t sinceFirst = now >= R->lateSince ? now - R->lateSince : 0;
        if(R->lastEntryAt != 0 && sinceEntry < TS_LAP_QUIET_MS && sinceFirst < TS_LAP_MAX_WAIT_MS) {
            return 0;
        }

        fresh = flow_merge_lap(&R->f, pos, w);
        if(!fresh) {
            return 0;
        }
    } else if(!flow_update_lap(&R->f, pos, w)) {
        return 0;
    }

    R->lateSince = 0;
    (void) fresh;
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        if(R->f.cl[i].used && !R->f.cl[i].done && !R->f.cl[i].late && !R->acked[i]) {
            R->inlineSince[i] = now ? now : 1; // messo in linea adesso, senza ancora un riscontro
        }
    }

    uint8_t ub[TS_BITMAP_BYTES];
    *nTitles = room_union(R, m->count, ub, 1);
    memcpy(R->lapUnion, ub, TS_BITMAP_BYTES);
    R->lapValid = 1;
    *nSkip = ts_skip_ranges(m, ub, skip, maxSkip);
    flow_set_skips(&R->f, skip, *nSkip);
    for(int i = 0; i < TS_MAX_CLIENTS; i++) {
        if(R->f.cl[i].used && R->f.cl[i].late) {
            flow_set_client_info(&R->f, i, R->cap[i], room_ride_ok(R, i));
        }
    }

    return 1;
}
