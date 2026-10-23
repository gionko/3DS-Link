#include "win.h"

#include <stdlib.h>
#include <string.h>

int win_init(ts_win* w, uint32_t window) {
    memset(w, 0, sizeof(*w));
    w->W = window;
    w->data = malloc((size_t) window * TS_BLOCK);
    w->len = calloc(window, sizeof(uint16_t));
    w->have = calloc(window, 1);
    if(w->data == NULL || w->len == NULL || w->have == NULL) {
        win_free(w);
        return -1;
    }

    return 0;
}

void win_free(ts_win* w) {
    free(w->data);
    free(w->len);
    free((void*) w->have);
    memset(w, 0, sizeof(*w));
}

void win_reset(ts_win* w, uint32_t cursor) {
    memset((void*) w->have, 0, w->W);
    __sync_synchronize();
    w->hi = cursor;
    w->cursor = cursor;
}

int win_put(ts_win* w, uint32_t block, const uint8_t* payload, size_t len) {
    uint32_t cur = w->cursor;
    if(block < cur) {
        w->statDup++;
        return 0;
    }

    if(block >= cur + w->W || len > TS_BLOCK) {
        w->statOut++;
        return -1;
    }

    uint32_t slot = block % w->W;
    if(w->have[slot]) {
        w->statDup++;
        return 0;
    }

    memcpy(w->data + (size_t) slot * TS_BLOCK, payload, len);
    w->len[slot] = (uint16_t) len;
    __sync_synchronize();
    w->have[slot] = 1;

    if(block > w->hi) {
        uint32_t g = block - w->hi;
        w->statGaps++;
        w->statGapBlocks += g;
        if(g > w->statGapMax) {
            w->statGapMax = g;
        }

        w->gapHist[g == 1 ? 0 : (g < 4 ? 1 : (g < 16 ? 2 : (g < 128 ? 3 : 4)))]++;
    }

    if(block + 1 > w->hi) {
        w->hi = block + 1;
    }

    w->statRecv++;
    return 1;
}

const uint8_t* win_peek(ts_win* w, size_t* len) {
    uint32_t slot = w->cursor % w->W;
    if(!w->have[slot]) {
        return NULL;
    }

    __sync_synchronize();
    *len = w->len[slot];
    return w->data + (size_t) slot * TS_BLOCK;
}

void win_pop(ts_win* w) {
    uint32_t slot = w->cursor % w->W;
    __sync_synchronize();
    w->have[slot] = 0;
    w->cursor = w->cursor + 1;
}

int win_missing(const ts_win* w, ts_range* out, int max) {
    int n = 0;
    uint32_t cur = w->cursor, hi = w->hi;
    uint32_t g = cur;
    while(g < hi && n < max) {
        if(w->have[g % w->W]) {
            g++;
            continue;
        }

        uint32_t start = g;
        while(g < hi && !w->have[g % w->W]) {
            g++;
        }

        out[n].start = start;
        out[n].count = g - start;
        n++;
    }

    return n;
}

void win_skip(ts_win* w, uint32_t from, uint32_t to) {
    if(w->cursor != from || to <= from) {
        return;
    }

    // Gli slot dei blocchi saltati si liberano: se il ricevente li aveva gia' ricevuti (un intervallo che un nuovo giro non invia piu') non
    // devono restare occupati, altrimenti il blocco che li riusa piu' avanti verrebbe preso per un doppio.
    uint32_t n = to - from;
    if(n > w->W) {
        n = w->W;
    }

    uint32_t slot = from % w->W;
    for(uint32_t i = 0; i < n; i++) {
        w->have[slot] = 0;
        slot = slot + 1 == w->W ? 0 : slot + 1;
    }

    __sync_synchronize();
    if(to > w->hi) {
        w->hi = to;
    }

    w->cursor = to;
}

int win_missing_skip(const ts_win* w, const ts_range* skip, int nSkip, ts_range* out, int max) {
    // Una sola scansione dal cursore a `hi`. Lo slot si tiene con un indice che avanza (niente `%` per blocco: sull'ARM11 la
    // divisione e' software) e i salti si seguono con un indice (nSkip e' ordinato): costo O(hi - cursore + nSkip), non O(... * nSkip).
    int n = 0;
    uint32_t hi = w->hi;
    uint32_t g = w->cursor;
    uint32_t slot = g % w->W;
    int si = 0;
    while(g < hi && n < max) {
        while(si < nSkip && skip[si].start + skip[si].count <= g) {
            si++;
        }

        if(si < nSkip && g >= skip[si].start) {
            uint32_t e = skip[si].start + skip[si].count; // dentro un intervallo saltato: lo si scavalca
            slot = (uint32_t) (((uint64_t) slot + (e - g)) % w->W);
            g = e;
            continue;
        }

        if(w->have[slot]) {
            g++;
            slot = slot + 1 == w->W ? 0 : slot + 1;
            continue;
        }

        uint32_t limit = (si < nSkip && skip[si].start < hi) ? skip[si].start : hi; // il buco finisce al salto successivo o a hi
        uint32_t start = g;
        while(g < limit && !w->have[slot]) {
            g++;
            slot = slot + 1 == w->W ? 0 : slot + 1;
        }

        out[n].start = start;
        out[n].count = g - start;
        n++;
    }

    return n;
}
