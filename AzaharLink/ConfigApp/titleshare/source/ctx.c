#include "ctx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aes.h"
#include "ciastream.h"
#include "hostcfg.h"
#include "sdcrypt.h"

static int read_file(const char* path, uint8_t** out, size_t* size, size_t maxSize) {
    FILE* f = fopen(path, "rb");
    if(f == NULL) {
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(n <= 0 || (size_t) n > maxSize) {
        fclose(f);
        return -2;
    }

    uint8_t* b = malloc((size_t) n);
    if(b == NULL || fread(b, 1, (size_t) n, f) != (size_t) n) {
        free(b);
        fclose(f);
        return -3;
    }

    fclose(f);
    *out = b;
    *size = (size_t) n;
    return 0;
}

static int load_boot9(uint8_t** out, size_t* size) {
    // il file lo cerca l'app (accanto a se' stessa, nella cartella di GodMode9, dove l'utente l'ha indicato) e qui arriva il percorso trovato (hostcfg.h)
    const char* path = hc_boot9_path();
    if(path[0] == 0) {
        return -1;
    }

    if(read_file(path, out, size, 0x10000) == 0) {
        if(*size == 0x10000 || *size == 0x8000) {
            return 0;
        }

        free(*out);
        *out = NULL;
    }

    return -1;
}

static int load_movable(uint8_t* out, size_t* size) {
    static const FS_ArchiveID archives[] = {ARCHIVE_NAND_RO, ARCHIVE_NAND_CTR_FS};
    for(size_t i = 0; i < sizeof(archives) / sizeof(archives[0]); i++) {
        Handle h = 0;
        if(R_FAILED(FSUSER_OpenFileDirectly(&h, archives[i], fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, "/private/movable.sed"), FS_OPEN_READ, 0))) {
            continue;
        }

        u32 read = 0;
        Result r = FSFILE_Read(h, &read, 0, out, 0x140);
        FSFILE_Close(h);
        if(R_SUCCEEDED(r) && read >= 0x120) {
            *size = read;
            return 0;
        }
    }

    // accanto all'app, poi nella cartella di GodMode9 (hostcfg.h)
    for(int i = 0; i < 2; i++) {
        char path[HC_PATH_MAX];
        if(hc_movable_candidate(i, path, sizeof(path)) != 0) {
            continue;
        }

        uint8_t* b = NULL;
        size_t n = 0;
        if(read_file(path, &b, &n, 0x140) == 0 && n >= 0x120) {
            memcpy(out, b, n);
            free(b);
            *size = n;
            return 0;
        }

        free(b);
    }

    return -1;
}

static int load_seeddb(ts_ctx* c) {
    char path[128];
    snprintf(path, sizeof(path), "/data/%s/sysdata/0001000f/00000000", c->id0);

    static const FS_ArchiveID archives[] = {ARCHIVE_NAND_RO, ARCHIVE_NAND_CTR_FS};
    for(size_t i = 0; i < sizeof(archives) / sizeof(archives[0]); i++) {
        Handle h = 0;
        if(R_FAILED(FSUSER_OpenFileDirectly(&h, archives[i], fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0))) {
            continue;
        }

        u64 fsize = 0;
        if(R_SUCCEEDED(FSFILE_GetSize(h, &fsize)) && fsize > 0 && fsize <= 0x800000) {
            uint8_t* b = malloc((size_t) fsize);
            u32 read = 0;
            if(b != NULL && R_SUCCEEDED(FSFILE_Read(h, &read, 0, b, (u32) fsize)) && read == fsize) {
                FSFILE_Close(h);
                c->seedDb = b;
                c->seedDbSize = (size_t) fsize;
                return 0;
            }

            free(b);
        }

        FSFILE_Close(h);
    }

    return -1;
}

void ctx_product_code(u64 tid, char out[16]) {
    memset(out, 0, 16);
    AM_GetTitleProductCode(MEDIATYPE_SD, tid, out);
    out[15] = 0;
}

int ctx_init(ts_ctx* c, char* msg, size_t msgSize) {
    memset(c, 0, sizeof(*c));
    aes_init();
    APT_CheckNew3DS(&c->isNew);

    msg[0] = 0;

    {
        // identificativo stabile della console per il mittente (riconosce un riavvio): non dipende dalle chiavi
        u16 root[256] = {0};
        if(R_SUCCEEDED(FSUSER_GetSdmcCtrRootPath((u8*) root, sizeof(root)))) {
            size_t k = 0;
            for(; k < sizeof(c->devId) - 1 && k < 255 && root[k] != 0; k++) {
                c->devId[k] = (char) root[k];
            }

            c->devId[k] = 0;
        }
    }

    // L'elenco dei titoli serve anche senza chiavi (per vedere cosa e' installato e ricevere).
    u32 count = 0;
    if(R_SUCCEEDED(AM_GetTitleCount(MEDIATYPE_SD, &count)) && count > 0) {
        c->titleIds = malloc(sizeof(u64) * count);
        u32 got = 0;
        if(c->titleIds != NULL && R_SUCCEEDED(AM_GetTitleList(&got, MEDIATYPE_SD, count, c->titleIds))) {
            c->titleCount = got;
        }
    }

    uint8_t* boot9 = NULL;
    size_t boot9Size = 0;
    uint8_t movable[0x140];
    size_t movableSize = 0;

    if(load_boot9(&boot9, &boot9Size) != 0) {
        snprintf(msg, msgSize, "boot9.bin non trovato: solo ricezione");
        return 0;
    }

    if(load_movable(movable, &movableSize) != 0) {
        free(boot9);
        snprintf(msg, msgSize, "movable.sed non leggibile: solo ricezione");
        return 0;
    }

    uint8_t key[16];
    int rc = sd_derive_key(boot9, boot9Size, movable, movableSize, key);
    c->haveNcchKey = sd_ncch_keyx(boot9, boot9Size, c->keyX2C) == 0;
    free(boot9);
    if(rc != 0) {
        snprintf(msg, msgSize, "derivazione della chiave fallita: solo ricezione");
        return 0;
    }

    aes_set_key(c->rk, key);
    memset(key, 0, sizeof(key));
    sd_id0(movable, c->id0);

    u16 root[256] = {0};
    if(R_FAILED(FSUSER_GetSdmcCtrRootPath((u8*) root, sizeof(root)))) {
        snprintf(msg, msgSize, "percorso ID0/ID1 non disponibile: solo ricezione");
        return 0;
    }

    char ascii[256];
    size_t n = 0;
    for(; n < 255 && root[n] != 0; n++) {
        ascii[n] = (char) root[n];
    }

    ascii[n] = 0;
    snprintf(c->ctrRoot, sizeof(c->ctrRoot), "sdmc:%s", ascii);
    if(strstr(c->ctrRoot, c->id0) == NULL) {
        snprintf(msg, msgSize, "l'ID0 della chiave non coincide con la SD: solo ricezione");
        return 0;
    }

    if(cia_load_certs_db(&c->certsDb, &c->certsDbSize) != 0) {
        snprintf(msg, msgSize, "certs.db non leggibile dal NAND: solo ricezione");
        return 0;
    }

    load_seeddb(c); // se manca, i titoli con seed non potranno essere inviati

    c->haveKeys = true;
    return 0;
}

void ctx_exit(ts_ctx* c) {
    free(c->certsDb);
    free(c->seedDb);
    free(c->titleIds);
    memset(c, 0, sizeof(*c));
}
