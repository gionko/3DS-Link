#include "builtin_keys.h"

#include <string.h>

#include "../titleshare/common/aes.h"
#include "builtin_keys_blob.h"

static int hex_digit(char c) {
    if(c >= '0' && c <= '9') {
        return c - '0';
    }
    if(c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if(c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// aes_init builds its tables once and is not safe to run in two threads at the same moment: the first caller does it, the others wait for the end of it
static void aes_ready(void) {
    static int state; // 0 = not yet, 1 = being done, 2 = done
    if(__atomic_load_n(&state, __ATOMIC_ACQUIRE) == 2) {
        return;
    }
    int expected = 0;
    if(__atomic_compare_exchange_n(&state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        aes_init();
        __atomic_store_n(&state, 2, __ATOMIC_RELEASE);
    } else {
        while(__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 2) {
        }
    }
}

int bk_get(const char* name, uint8_t out[16]) {
    memset(out, 0, 16);
    if(name == NULL || sizeof(bk_blob) > 256) {
        return -1;
    }

    // the blob, in the clear: AES-128 in CTR mode, a key of zeros and a counter of zeros (the same call that reads a title's SD layer)
    uint8_t text[256];
    memcpy(text, bk_blob, sizeof(bk_blob));
    uint8_t key[16] = {0}, ctr[16] = {0};
    uint32_t rk[44];
    aes_ready();
    aes_set_key(rk, key);
    aes_ctr_crypt(rk, ctr, 0, text, sizeof(bk_blob));

    // "name=32 hex digits", one per line
    const size_t nlen = strlen(name);
    size_t i = 0;
    while(i < sizeof(bk_blob)) {
        size_t end = i;
        while(end < sizeof(bk_blob) && text[end] != '\n') {
            end++;
        }
        if(end - i == nlen + 1 + 32 && memcmp(text + i, name, nlen) == 0 && text[i + nlen] == '=') {
            uint8_t value[16];
            for(int b = 0; b < 16; b++) {
                const int hi = hex_digit((char) text[i + nlen + 1 + 2 * b]);
                const int lo = hex_digit((char) text[i + nlen + 2 + 2 * b]);
                if(hi < 0 || lo < 0) {
                    return -1;
                }
                value[b] = (uint8_t) ((hi << 4) | lo);
            }
            memcpy(out, value, 16);
            return 0;
        }
        i = end + 1;
    }
    return -1;
}
