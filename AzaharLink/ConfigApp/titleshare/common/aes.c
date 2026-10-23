#include "aes.h"

#include <string.h>

static uint8_t sbox[256];
static uint32_t T0[256], T1[256], T2[256], T3[256];
static int inited;

static uint8_t rotl8(uint8_t x, int s) {
    return (uint8_t) ((x << s) | (x >> (8 - s)));
}

static uint8_t xt(uint8_t a) {
    return (uint8_t) ((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
}

void aes_init(void) {
    if(inited) {
        return;
    }

    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t) (p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));

        q ^= (uint8_t) (q << 1);
        q ^= (uint8_t) (q << 2);
        q ^= (uint8_t) (q << 4);
        if(q & 0x80) {
            q ^= 0x09;
        }

        sbox[p] = (uint8_t) (q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
    } while(p != 1);
    sbox[0] = 0x63;

    for(int i = 0; i < 256; i++) {
        uint8_t s = sbox[i];
        uint8_t s2 = xt(s);
        uint8_t s3 = (uint8_t) (s2 ^ s);
        uint32_t t = ((uint32_t) s2 << 24) | ((uint32_t) s << 16) | ((uint32_t) s << 8) | s3;
        T0[i] = t;
        T1[i] = (t >> 8) | (t << 24);
        T2[i] = (t >> 16) | (t << 16);
        T3[i] = (t >> 24) | (t << 8);
    }

    inited = 1;
}

static uint32_t sub_word(uint32_t w) {
    return ((uint32_t) sbox[w >> 24] << 24) | ((uint32_t) sbox[(w >> 16) & 0xFF] << 16) |
           ((uint32_t) sbox[(w >> 8) & 0xFF] << 8) | sbox[w & 0xFF];
}

void aes_set_key(uint32_t rk[44], const uint8_t key[16]) {
    for(int i = 0; i < 4; i++) {
        rk[i] = ((uint32_t) key[4 * i] << 24) | ((uint32_t) key[4 * i + 1] << 16) | ((uint32_t) key[4 * i + 2] << 8) | key[4 * i + 3];
    }

    uint8_t rcon = 1;
    for(int i = 4; i < 44; i++) {
        uint32_t t = rk[i - 1];
        if(i % 4 == 0) {
            t = sub_word((t << 8) | (t >> 24)) ^ ((uint32_t) rcon << 24);
            rcon = xt(rcon);
        }

        rk[i] = rk[i - 4] ^ t;
    }
}

// Cifra un blocco: ingresso e uscita come 4 parole big-endian.
static inline void encrypt_words(const uint32_t* rk, uint32_t s0, uint32_t s1, uint32_t s2, uint32_t s3, uint32_t out[4]) {
    s0 ^= rk[0];
    s1 ^= rk[1];
    s2 ^= rk[2];
    s3 ^= rk[3];

    for(int r = 1; r < 10; r++) {
        const uint32_t* k = rk + 4 * r;
        uint32_t t0 = T0[s0 >> 24] ^ T1[(s1 >> 16) & 0xFF] ^ T2[(s2 >> 8) & 0xFF] ^ T3[s3 & 0xFF] ^ k[0];
        uint32_t t1 = T0[s1 >> 24] ^ T1[(s2 >> 16) & 0xFF] ^ T2[(s3 >> 8) & 0xFF] ^ T3[s0 & 0xFF] ^ k[1];
        uint32_t t2 = T0[s2 >> 24] ^ T1[(s3 >> 16) & 0xFF] ^ T2[(s0 >> 8) & 0xFF] ^ T3[s1 & 0xFF] ^ k[2];
        uint32_t t3 = T0[s3 >> 24] ^ T1[(s0 >> 16) & 0xFF] ^ T2[(s1 >> 8) & 0xFF] ^ T3[s2 & 0xFF] ^ k[3];
        s0 = t0;
        s1 = t1;
        s2 = t2;
        s3 = t3;
    }

    const uint32_t* k = rk + 40;
    out[0] = (((uint32_t) sbox[s0 >> 24] << 24) | ((uint32_t) sbox[(s1 >> 16) & 0xFF] << 16) | ((uint32_t) sbox[(s2 >> 8) & 0xFF] << 8) | sbox[s3 & 0xFF]) ^ k[0];
    out[1] = (((uint32_t) sbox[s1 >> 24] << 24) | ((uint32_t) sbox[(s2 >> 16) & 0xFF] << 16) | ((uint32_t) sbox[(s3 >> 8) & 0xFF] << 8) | sbox[s0 & 0xFF]) ^ k[1];
    out[2] = (((uint32_t) sbox[s2 >> 24] << 24) | ((uint32_t) sbox[(s3 >> 16) & 0xFF] << 16) | ((uint32_t) sbox[(s0 >> 8) & 0xFF] << 8) | sbox[s1 & 0xFF]) ^ k[2];
    out[3] = (((uint32_t) sbox[s3 >> 24] << 24) | ((uint32_t) sbox[(s0 >> 16) & 0xFF] << 16) | ((uint32_t) sbox[(s1 >> 8) & 0xFF] << 8) | sbox[s2 & 0xFF]) ^ k[3];
}

void aes_ctr_crypt(const uint32_t rk[44], const uint8_t ctr0[16], uint64_t offset, uint8_t* data, size_t len) {
    uint32_t c[4];
    for(int i = 0; i < 4; i++) {
        c[i] = ((uint32_t) ctr0[4 * i] << 24) | ((uint32_t) ctr0[4 * i + 1] << 16) | ((uint32_t) ctr0[4 * i + 2] << 8) | ctr0[4 * i + 3];
    }

    // contatore = ctr0 + offset / 16 (somma a 128 bit)
    uint64_t blocks = offset >> 4;
    uint64_t lo = ((uint64_t) c[2] << 32) | c[3];
    uint64_t newLo = lo + blocks;
    if(newLo < lo) {
        uint64_t hi = ((uint64_t) c[0] << 32) | c[1];
        hi++;
        c[0] = (uint32_t) (hi >> 32);
        c[1] = (uint32_t) hi;
    }

    c[2] = (uint32_t) (newLo >> 32);
    c[3] = (uint32_t) newLo;

    uint32_t ks[4];
    while(len >= 16) {
        encrypt_words(rk, c[0], c[1], c[2], c[3], ks);

        if(((uintptr_t) data & 3) == 0) {
            uint32_t* d = (uint32_t*) data;
            d[0] ^= __builtin_bswap32(ks[0]);
            d[1] ^= __builtin_bswap32(ks[1]);
            d[2] ^= __builtin_bswap32(ks[2]);
            d[3] ^= __builtin_bswap32(ks[3]);
        } else {
            for(int i = 0; i < 16; i++) {
                data[i] ^= (uint8_t) (ks[i >> 2] >> (24 - 8 * (i & 3)));
            }
        }

        data += 16;
        len -= 16;

        if(++c[3] == 0 && ++c[2] == 0 && ++c[1] == 0) {
            ++c[0];
        }
    }

    if(len > 0) {
        encrypt_words(rk, c[0], c[1], c[2], c[3], ks);
        for(size_t i = 0; i < len; i++) {
            data[i] ^= (uint8_t) (ks[i >> 2] >> (24 - 8 * (i & 3)));
        }
    }
}
