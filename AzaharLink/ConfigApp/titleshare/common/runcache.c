#include "runcache.h"

#include <string.h>

size_t ts_rc_mem_size(uint32_t nSlots, uint32_t runBlocks) {
    return (size_t) nSlots * runBlocks * TS_BLOCK;
}

int ts_rc_init(ts_runcache* c, const ts_manifest* m, uint8_t* mem, uint32_t nSlots, uint32_t runBlocks, ts_rc_read_fn read, void* ctx) {
    if(c == NULL || m == NULL || mem == NULL || read == NULL || nSlots < 1 || nSlots > TS_RC_MAX_SLOTS || runBlocks < 1 || runBlocks > TS_RC_MAX_RUN_BLOCKS) {
        return -1;
    }

    memset(c, 0, sizeof(*c)); // tutte le corse vuote (count = 0), hint = 0
    c->m = m;
    c->mem = mem;
    c->read = read;
    c->ctx = ctx;
    c->nSlots = nSlots;
    c->runBlocks = runBlocks;
    return 0;
}

void ts_rc_invalidate_all(ts_runcache* c) {
    for(uint32_t i = 0; i < c->nSlots; i++) {
        c->s[i].count = 0;
    }
}

void ts_rc_invalidate_title(ts_runcache* c, int title) {
    if(title < 0 || title >= c->m->count) {
        return;
    }

    // una corsa non esce mai dal suo titolo: basta guardare dove comincia
    const ts_title* t = &c->m->t[title];
    for(uint32_t i = 0; i < c->nSlots; i++) {
        if(c->s[i].count > 0 && (uint32_t) (c->s[i].start - t->firstBlock) < t->blocks) {
            c->s[i].count = 0;
        }
    }
}

const uint8_t* ts_rc_find(ts_runcache* c, uint32_t g) {
    const ts_rc_slot* s = c->s;
    uint32_t i = c->hint;
    // (g - start) senza segno < count: una sola prova per corsa, e le corse vuote (count = 0) non rispondono mai
    if((uint32_t) (g - s[i].start) >= s[i].count) {
        i = 0;
        while(i < c->nSlots && (uint32_t) (g - s[i].start) >= s[i].count) {
            i++;
        }

        if(i == c->nSlots) {
            return NULL;
        }
    }

    c->s[i].use = ++c->tick;
    c->hint = i;
    c->hits++;
    return c->mem + ((size_t) i * c->runBlocks + (size_t) (g - s[i].start)) * TS_BLOCK;
}

// Una corsa vuota se c'e', altrimenti la meno usata di recente (eta' = tick - use, la piu' grande).
static uint32_t victim(const ts_runcache* c) {
    uint32_t v = 0, oldest = 0;
    for(uint32_t i = 0; i < c->nSlots; i++) {
        if(c->s[i].count == 0) {
            return i;
        }

        uint32_t age = c->tick - c->s[i].use;
        if(i == 0 || age > oldest) {
            v = i;
            oldest = age;
        }
    }

    return v;
}

const uint8_t* ts_rc_fill(ts_runcache* c, uint32_t g, ts_rc_info* info) {
    ts_rc_info local;
    if(info == NULL) {
        info = &local;
    }

    info->how = TS_RC_RANGE;
    info->title = -1;
    info->count = 0;
    info->slot = 0;

    int ti;
    uint64_t off;
    size_t len;
    if(!ts_block_info(c->m, g, &ti, &off, &len)) {
        c->range++;
        return NULL;
    }

    // la corsa comincia in g e non esce dal titolo (come la lettura da 16 blocchi di prima)
    const ts_title* t = &c->m->t[ti];
    uint32_t left = t->firstBlock + t->blocks - g;
    uint32_t cnt = left < c->runBlocks ? left : c->runBlocks;
    uint64_t remain = t->size - off;
    size_t bytes = (uint64_t) cnt * TS_BLOCK < remain ? (size_t) cnt * TS_BLOCK : (size_t) remain;

    uint32_t v = victim(c);
    ts_rc_slot old = c->s[v];
    uint8_t* dst = c->mem + (size_t) v * c->runBlocks * TS_BLOCK;
    ts_rc_req rq = {g, cnt, ti, off, bytes};
    info->title = ti;
    info->count = cnt;
    info->slot = v;
    c->s[v].count = 0; // durante la lettura la corsa e' vuota: dst si riempie a pezzi e non deve mai valere se la lettura si interrompe

    int rc = c->read(c->ctx, &rq, dst);
    if(rc == TS_RC_OK) {
        c->s[v].start = g;
        c->s[v].count = cnt;
        c->s[v].use = ++c->tick;
        c->hint = v;
        c->fills++;
        c->blocksRead += cnt;
        info->how = TS_RC_FILLED;
        return dst;
    }

    if(rc == TS_RC_NOOP) {
        c->s[v] = old; // dst intatto: la corsa di prima vale ancora
        c->noops++;
    } else {
        c->failed++; // rimane vuota: sara' la prima a essere riusata
    }

    info->how = TS_RC_FAILED;
    return NULL;
}

const uint8_t* ts_rc_get(ts_runcache* c, uint32_t g, ts_rc_info* info) {
    const uint8_t* p = ts_rc_find(c, g);
    if(p != NULL) {
        if(info != NULL) {
            info->how = TS_RC_HIT;
            info->title = -1;
            info->count = 0;
            info->slot = c->hint;
        }

        return p;
    }

    return ts_rc_fill(c, g, info);
}

int ts_rc_slot_info(const ts_runcache* c, uint32_t i, uint32_t* start, uint32_t* count, uint32_t* use, const uint8_t** data) {
    if(i >= c->nSlots) {
        return 0;
    }

    if(start != NULL) {
        *start = c->s[i].start;
    }

    if(count != NULL) {
        *count = c->s[i].count;
    }

    if(use != NULL) {
        *use = c->s[i].use;
    }

    if(data != NULL) {
        *data = c->mem + (size_t) i * c->runBlocks * TS_BLOCK;
    }

    return c->s[i].count > 0;
}
