#include "sdcrypt.h"

#include <stdio.h>
#include <string.h>

#include "sha256.h"
#include "../../titleshare_host/builtin_keys.h"

typedef struct {
    uint64_t hi, lo;
} u128;

static u128 load_be(const uint8_t* p) {
    u128 r = {0, 0};
    for(int i = 0; i < 8; i++) {
        r.hi = (r.hi << 8) | p[i];
        r.lo = (r.lo << 8) | p[8 + i];
    }

    return r;
}

static void store_be(u128 v, uint8_t* p) {
    for(int i = 0; i < 8; i++) {
        p[i] = (uint8_t) (v.hi >> (56 - 8 * i));
        p[8 + i] = (uint8_t) (v.lo >> (56 - 8 * i));
    }
}

static u128 rol128(u128 v, unsigned r) {
    r &= 127;
    if(r >= 64) {
        u128 s = {v.lo, v.hi};
        v = s;
        r -= 64;
    }

    if(r == 0) {
        return v;
    }

    u128 o = {(v.hi << r) | (v.lo >> (64 - r)), (v.lo << r) | (v.hi >> (64 - r))};
    return o;
}

int sd_derive_key(const uint8_t* boot9, size_t boot9Size, const uint8_t* movable, size_t movableSize, uint8_t key[16]) {
    size_t keyblob;
    if(boot9Size == 0x10000) {
        keyblob = 0x5860 + 0x8000;
    } else if(boot9Size == 0x8000) {
        keyblob = 0x5860;
    } else {
        return -1;
    }

    if(movableSize < 0x120) {
        return -2;
    }

    // Terza chiave X del keyblob (slot 0x2C, 0x30, 0x34).
    u128 x = load_be(boot9 + keyblob + 0x170 + 0x20);
    u128 y = load_be(movable + 0x110);

    // Scrambler hardware AES del 3DS: rol((rol(X, 2) ^ Y) + C, 87). C e' la costante del generatore di chiavi (i valori di default di Azahar: builtin_keys.h).
    uint8_t cbytes[16];
    if(bk_get(BK_GENERATOR_CONSTANT, cbytes) != 0) {
        return -9;
    }
    const u128 C = load_be(cbytes);
    u128 t = rol128(x, 2);
    t.hi ^= y.hi;
    t.lo ^= y.lo;

    u128 s;
    s.lo = t.lo + C.lo;
    s.hi = t.hi + C.hi + (s.lo < t.lo ? 1 : 0);

    store_be(rol128(s, 87), key);
    return 0;
}

void sd_id0(const uint8_t* movable, char out[33]) {
    uint8_t h[32];
    sha256(movable + 0x110, 16, h);

    for(int i = 0; i < 4; i++) {
        uint32_t w = (uint32_t) h[4 * i] | ((uint32_t) h[4 * i + 1] << 8) | ((uint32_t) h[4 * i + 2] << 16) | ((uint32_t) h[4 * i + 3] << 24);
        snprintf(out + 8 * i, 9, "%08x", (unsigned int) w);
    }

    out[32] = 0;
}

void sd_path_ctr(const char* path, uint8_t ctr[16]) {
    uint8_t buf[512];
    size_t n = 0;

    for(const char* p = path; *p != 0 && n + 4 <= sizeof(buf); p++) {
        char c = *p == '\\' ? '/' : *p;
        if(c >= 'A' && c <= 'Z') {
            c = (char) (c - 'A' + 'a');
        }

        buf[n++] = (uint8_t) c;
        buf[n++] = 0;
    }

    buf[n++] = 0;
    buf[n++] = 0;

    uint8_t h[32];
    sha256(buf, n, h);

    for(int i = 0; i < 16; i++) {
        ctr[i] = h[i] ^ h[16 + i];
    }
}

int sd_ncch_keyx(const uint8_t* boot9, size_t boot9Size, uint8_t x[16]) {
    size_t keyblob;
    if(boot9Size == 0x10000) {
        keyblob = 0x5860 + 0x8000;
    } else if(boot9Size == 0x8000) {
        keyblob = 0x5860;
    } else {
        return -1;
    }

    memcpy(x, boot9 + keyblob + 0x170, 16); // prima chiave X del keyblob = slot 0x2C
    return 0;
}

void ncch_scramble(const uint8_t x[16], const uint8_t y[16], uint8_t key[16]) {
    uint8_t cbytes[16];
    if(bk_get(BK_GENERATOR_CONSTANT, cbytes) != 0) {
        memset(key, 0, 16);
        return;
    }
    const u128 C = load_be(cbytes);
    u128 t = rol128(load_be(x), 2);
    u128 yy = load_be(y);
    t.hi ^= yy.hi;
    t.lo ^= yy.lo;

    u128 s;
    s.lo = t.lo + C.lo;
    s.hi = t.hi + C.hi + (s.lo < t.lo ? 1 : 0);
    store_be(rol128(s, 87), key);
}
