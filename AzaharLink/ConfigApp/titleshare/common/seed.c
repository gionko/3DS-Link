#include "seed.h"

#include <string.h>

#include "sha256.h"

int seed_matches(const uint8_t seed[16], uint64_t tid, const uint8_t check[4]) {
    uint8_t in[24], h[32];
    memcpy(in, seed, 16);
    for(int i = 0; i < 8; i++) {
        in[16 + i] = (uint8_t) (tid >> (8 * i));
    }

    sha256(in, sizeof(in), h);
    return memcmp(h, check, 4) == 0;
}

int seed_find(const uint8_t* db, size_t size, uint64_t tid, const uint8_t check[4], uint8_t seed[16], size_t* where) {
    // Prima le finestre allineate a 16 byte (come i seed nella tabella), poi, se serve, tutte le altre.
    for(int pass = 0; pass < 2; pass++) {
        size_t step = pass == 0 ? 16 : 1;
        for(size_t off = 0; off + 16 <= size; off += step) {
            if(pass == 1 && (off & 15) == 0) {
                continue;
            }

            if(seed_matches(db + off, tid, check)) {
                memcpy(seed, db + off, 16);
                *where = off;
                return 0;
            }
        }
    }

    return -1;
}
