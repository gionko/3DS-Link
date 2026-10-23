#include "ncchname.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

#include "aes.h"
#include "contentpath.h"
#include "sdcrypt.h"

static uint32_t le32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t* p) {
    return (uint16_t) ((p[0] << 8) | p[1]);
}

// Legge len byte a partire da off e toglie lo strato SD (off deve essere multiplo di 16).
static int read_sd(FILE* f, const uint32_t sdrk[44], const uint8_t sdctr[16], uint64_t off, uint8_t* buf, size_t len) {
    if(fseeko(f, (off_t) off, SEEK_SET) != 0 || fread(buf, 1, len, f) != len) {
        return -1;
    }

    aes_ctr_crypt(sdrk, sdctr, off, buf, len);
    return 0;
}

// UTF-16 (little-endian, terminata da zero) -> UTF-8. Ritorna i byte scritti.
static size_t utf16_to_utf8_simple(const uint8_t* in, size_t maxUnits, char* out, size_t outSize) {
    size_t o = 0;
    for(size_t i = 0; i < maxUnits && o + 4 < outSize; i++) {
        uint32_t u = (uint32_t) in[2 * i] | ((uint32_t) in[2 * i + 1] << 8);
        if(u == 0) {
            break;
        }

        if(u >= 0xD800 && u < 0xDC00 && i + 1 < maxUnits) {
            uint32_t lo = (uint32_t) in[2 * i + 2] | ((uint32_t) in[2 * i + 3] << 8);
            if(lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }

        if(u == '\n' || u == '\r') {
            u = ' ';
        }

        if(u < 0x80) {
            out[o++] = (char) u;
        } else if(u < 0x800) {
            out[o++] = (char) (0xC0 | (u >> 6));
            out[o++] = (char) (0x80 | (u & 0x3F));
        } else if(u < 0x10000) {
            out[o++] = (char) (0xE0 | (u >> 12));
            out[o++] = (char) (0x80 | ((u >> 6) & 0x3F));
            out[o++] = (char) (0x80 | (u & 0x3F));
        } else {
            out[o++] = (char) (0xF0 | (u >> 18));
            out[o++] = (char) (0x80 | ((u >> 12) & 0x3F));
            out[o++] = (char) (0x80 | ((u >> 6) & 0x3F));
            out[o++] = (char) (0x80 | (u & 0x3F));
        }
    }

    out[o] = 0;
    return o;
}

int ncch_read_name(const uint32_t sdrk[44], const uint8_t keyX[16], const char* ctrRoot, uint64_t tid, int language, char* out, size_t outSize) {
    out[0] = 0;
    uint32_t hi = (uint32_t) (tid >> 32), lo = (uint32_t) tid;
    char contentDir[512], contentRel[512];
    snprintf(contentDir, sizeof(contentDir), "%s/title/%08" PRIx32 "/%08" PRIx32 "/content", ctrRoot, hi, lo);
    snprintf(contentRel, sizeof(contentRel), "/title/%08" PRIx32 "/%08" PRIx32 "/content", hi, lo);

    // TMD con il numero piu' alto
    DIR* d = opendir(contentDir);
    if(d == NULL) {
        return -1;
    }

    char tmdName[64] = {0};
    struct dirent* e;
    while((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if(n > 4 && strcasecmp(e->d_name + n - 4, ".tmd") == 0 && n < sizeof(tmdName) && strcmp(e->d_name, tmdName) > 0) {
            strcpy(tmdName, e->d_name);
        }
    }

    closedir(d);
    if(tmdName[0] == 0) {
        return -2;
    }

    char path[800], rel[800];
    snprintf(path, sizeof(path), "%s/%s", contentDir, tmdName);
    snprintf(rel, sizeof(rel), "%s/%s", contentRel, tmdName);

    FILE* f = fopen(path, "rb");
    if(f == NULL) {
        return -3;
    }

    static uint8_t tmd[0x20000];
    size_t tmdSize = fread(tmd, 1, sizeof(tmd), f);
    fclose(f);
    if(tmdSize < 0x200) {
        return -3;
    }

    uint8_t ctr[16];
    sd_path_ctr(rel, ctr);
    aes_ctr_crypt(sdrk, ctr, 0, tmd, tmdSize);

    uint32_t sigType = be32(tmd);
    size_t sigSize = sigType == 0x10003 ? 0x200 : sigType == 0x10004 ? 0x100 : sigType == 0x10005 ? 0x3C : 0;
    if(sigSize == 0) {
        return -4;
    }

    size_t hb = (4 + sigSize + 0x3F) & ~(size_t) 0x3F;
    int n = be16(tmd + hb + 0x9E);
    size_t cr = hb + 0xC4 + 0x24 * 64;
    if(n < 1 || cr + 0x30 * (size_t) n > tmdSize) {
        return -4;
    }

    // content con indice 0 (NCCH principale)
    uint32_t cid = 0;
    int found = 0;
    for(int i = 0; i < n; i++) {
        const uint8_t* r = tmd + cr + 0x30 * i;
        if(be16(r + 4) == 0) {
            cid = be32(r);
            found = 1;
            break;
        }
    }

    if(!found) {
        return -5;
    }

    content_dirs dirs;
    content_dirs_scan(&dirs, contentDir);
    int sub = -1;
    uint64_t fsz = 0;
    if(content_locate(&dirs, contentDir, contentRel, cid, path, sizeof(path), rel, sizeof(rel), &sub, &fsz) != 0) {
        return -6;
    }

    f = fopen(path, "rb");
    if(f == NULL) {
        return -6;
    }

    uint8_t sdctr[16];
    sd_path_ctr(rel, sdctr);

    int rc = -7;
    uint8_t hdr[0x200];
    uint32_t nrk[44];
    uint8_t nctr[16];
    uint8_t exh[0x200];
    static uint8_t smdh[0x2008];

    if(read_sd(f, sdrk, sdctr, 0, hdr, sizeof(hdr)) != 0 || memcmp(hdr + 0x100, "NCCH", 4) != 0) {
        goto done;
    }

    uint64_t exefsOff = (uint64_t) le32(hdr + 0x1A0) * 0x200, exefsSize = (uint64_t) le32(hdr + 0x1A4) * 0x200;
    if(exefsSize < 0x200) {
        rc = -8; // niente ExeFS (DLC)
        goto done;
    }

    int noCrypto = (hdr[0x18F] & 0x04) != 0;
    if(!noCrypto) {
        uint8_t key[16];
        ncch_scramble(keyX, hdr, key); // KeyY = primi 16 byte della firma
        aes_set_key(nrk, key);
        memset(key, 0, sizeof(key));

        // contatore: ID della partizione (big-endian), tipo 2 = ExeFS
        for(int i = 0; i < 8; i++) {
            nctr[i] = hdr[0x108 + 7 - i];
        }

        nctr[8] = 2;
        memset(nctr + 9, 0, 7);
    }

    if(read_sd(f, sdrk, sdctr, exefsOff, exh, sizeof(exh)) != 0) {
        rc = -9;
        goto done;
    }

    if(!noCrypto) {
        aes_ctr_crypt(nrk, nctr, 0, exh, sizeof(exh));
    }

    uint32_t iconOff = 0, iconSize = 0;
    for(int i = 0; i < 10; i++) {
        const uint8_t* en = exh + 16 * i;
        if(memcmp(en, "icon\0\0\0\0", 8) == 0) {
            iconOff = le32(en + 8);
            iconSize = le32(en + 12);
        }
    }

    if(iconSize < 0x2008) {
        rc = -10;
        goto done;
    }

    uint64_t inEx = 0x200 + (uint64_t) iconOff; // posizione dell'icona dentro l'ExeFS
    if(read_sd(f, sdrk, sdctr, exefsOff + inEx, smdh, sizeof(smdh)) != 0) {
        rc = -11;
        goto done;
    }

    if(!noCrypto) {
        aes_ctr_crypt(nrk, nctr, inEx, smdh, sizeof(smdh));
    }

    if(memcmp(smdh, "SMDH", 4) != 0) {
        rc = -12;
        goto done;
    }

    static const int fallback[] = {1, 0, 2, 3, 4, 5, 8, 9};
    int order[1 + sizeof(fallback) / sizeof(fallback[0])];
    order[0] = (language >= 0 && language < 16) ? language : 1;
    for(size_t i = 0; i < sizeof(fallback) / sizeof(fallback[0]); i++) {
        order[1 + i] = fallback[i];
    }

    rc = -13;
    for(size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        const uint8_t* t = smdh + 8 + 0x200 * order[i];
        if(t[0] != 0 || t[1] != 0) {
            utf16_to_utf8_simple(t, 0x40, out, outSize);
            rc = out[0] ? 0 : -13;
            break;
        }
    }

done:
    fclose(f);
    return rc;
}
