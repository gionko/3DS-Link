#include "catcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CC_MAGIC 0x43435354u // "TSCC"
#define CC_FORMAT 1u

typedef struct {
    uint32_t magic, format, lang, count, check; // check: FNV-1a delle voci
} cc_hdr;

typedef struct {
    uint8_t tid[8];
    uint8_t ver[2];
    char name[48];
} cc_rec; // 58 byte, senza padding (tutti byte): stesso formato su 3DS e su PC

static uint32_t fnv(uint32_t h, const uint8_t* p, size_t n) {
    for(size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }

    return h;
}

void catcache_init(ts_catcache* c, int lang) {
    memset(c, 0, sizeof(*c));
    c->lang = lang;
}

void catcache_free(ts_catcache* c) {
    free(c->e);
    c->e = NULL;
    c->n = c->cap = 0;
}

int catcache_load(ts_catcache* c, const char* path) {
    FILE* f = fopen(path, "rb");
    if(f == NULL) {
        return 0;
    }

    cc_hdr h;
    if(fread(&h, sizeof(h), 1, f) != 1 || h.magic != CC_MAGIC || h.format != CC_FORMAT || h.lang != (uint32_t) c->lang || h.count > 100000u) {
        fclose(f);
        return 0;
    }

    cc_rec* recs = (cc_rec*) malloc(sizeof(cc_rec) * (h.count ? h.count : 1));
    if(recs == NULL) {
        fclose(f);
        return 0;
    }

    int ok = h.count == 0 || fread(recs, sizeof(cc_rec), h.count, f) == h.count;
    fclose(f);
    uint32_t chk = 2166136261u;
    if(ok) {
        chk = fnv(chk, (const uint8_t*) recs, sizeof(cc_rec) * h.count);
    }

    if(!ok || chk != h.check) {
        free(recs);
        return 0;
    }

    int loaded = 0;
    for(uint32_t i = 0; i < h.count; i++) {
        uint64_t tid = 0;
        for(int k = 7; k >= 0; k--) {
            tid = (tid << 8) | recs[i].tid[k];
        }

        uint16_t ver = (uint16_t) (recs[i].ver[0] | (recs[i].ver[1] << 8));
        char name[48];
        memcpy(name, recs[i].name, sizeof(name));
        name[47] = 0;
        if(catcache_put(c, tid, ver, name) == 0) {
            loaded++;
        }
    }

    free(recs);
    return loaded;
}

const char* catcache_get(const ts_catcache* c, uint64_t tid, uint16_t ver) {
    for(int i = 0; i < c->n; i++) {
        if(c->e[i].tid == tid) {
            return c->e[i].ver == ver ? c->e[i].name : NULL;
        }
    }

    return NULL;
}

int catcache_put(ts_catcache* c, uint64_t tid, uint16_t ver, const char* name) {
    for(int i = 0; i < c->n; i++) {
        if(c->e[i].tid == tid) {
            c->e[i].ver = ver;
            snprintf(c->e[i].name, sizeof(c->e[i].name), "%s", name);
            return 0;
        }
    }

    if(c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 64;
        ts_ccent* ne = (ts_ccent*) realloc(c->e, sizeof(ts_ccent) * (size_t) nc);
        if(ne == NULL) {
            return -1;
        }

        c->e = ne;
        c->cap = nc;
    }

    ts_ccent* e = &c->e[c->n++];
    e->tid = tid;
    e->ver = ver;
    snprintf(e->name, sizeof(e->name), "%s", name);
    return 0;
}

int catcache_save(const ts_catcache* c, const char* path) {
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if(f == NULL) {
        return -1;
    }

    cc_rec* recs = (cc_rec*) calloc((size_t) (c->n ? c->n : 1), sizeof(cc_rec));
    if(recs == NULL) {
        fclose(f);
        remove(tmp);
        return -1;
    }

    for(int i = 0; i < c->n; i++) {
        for(int k = 0; k < 8; k++) {
            recs[i].tid[k] = (uint8_t) (c->e[i].tid >> (8 * k));
        }

        recs[i].ver[0] = (uint8_t) c->e[i].ver;
        recs[i].ver[1] = (uint8_t) (c->e[i].ver >> 8);
        snprintf(recs[i].name, sizeof(recs[i].name), "%s", c->e[i].name);
    }

    cc_hdr h = {CC_MAGIC, CC_FORMAT, (uint32_t) c->lang, (uint32_t) c->n, fnv(2166136261u, (const uint8_t*) recs, sizeof(cc_rec) * (size_t) c->n)};
    int ok = fwrite(&h, sizeof(h), 1, f) == 1 && (c->n == 0 || fwrite(recs, sizeof(cc_rec), (size_t) c->n, f) == (size_t) c->n);
    ok = fclose(f) == 0 && ok;
    free(recs);
    if(!ok) {
        remove(tmp);
        return -1;
    }

    remove(path); // su sdmc rename non sovrascrive
    if(rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }

    return 0;
}
