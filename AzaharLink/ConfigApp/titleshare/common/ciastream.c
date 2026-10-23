#include "ciastream.h"

#ifdef __3DS__
#include <3ds.h>
#endif
#include <dirent.h>
#include <strings.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "aes.h"
#include "sdcrypt.h"

#ifdef __3DS__
int cia_load_certs_db(uint8_t** out, size_t* size) {
    static const FS_ArchiveID archives[] = {ARCHIVE_NAND_RO, ARCHIVE_NAND_CTR_FS};

    for(size_t i = 0; i < sizeof(archives) / sizeof(archives[0]); i++) {
        Handle h = 0;
        if(R_FAILED(FSUSER_OpenFileDirectly(&h, archives[i], fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, "/dbs/certs.db"), FS_OPEN_READ, 0))) {
            continue;
        }

        u64 fsize = 0;
        if(R_SUCCEEDED(FSFILE_GetSize(h, &fsize)) && fsize > 0 && fsize <= 0x100000) {
            uint8_t* b = malloc((size_t) fsize);
            u32 read = 0;
            if(b != NULL && R_SUCCEEDED(FSFILE_Read(h, &read, 0, b, (u32) fsize)) && read == fsize) {
                FSFILE_Close(h);
                *out = b;
                *size = (size_t) fsize;
                return 0;
            }

            free(b);
        }

        FSFILE_Close(h);
    }

    return -1;
}
#endif

static int ends_with(const char* s, const char* suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcasecmp(s + a - b, suffix) == 0;
}

static int read_whole(const char* path, uint8_t** out, size_t* size) {
    FILE* f = fopen(path, "rb");
    if(f == NULL) {
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(n <= 0 || n > 0x100000) {
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

static void seterr(cia_stream* s, const char* msg) {
    snprintf(s->err, sizeof(s->err), "%s", msg);
}

int cia_stream_open(cia_stream* s, uint64_t tid, const char* ctrRoot, const uint32_t rk[44], const uint8_t* certsDb, size_t certsDbSize) {
    uint64_t kF = s->tkFread, kA = s->tkAes, kB = s->bytesRead; // le statistiche di lettura sono cumulative: non ripartono a ogni titolo
    memset(s, 0, sizeof(*s));
    s->tkFread = kF;
    s->tkAes = kA;
    s->bytesRead = kB;
    s->rk = rk;
    s->fileIdx = -1;

    uint32_t hi = (uint32_t) (tid >> 32), lo = (uint32_t) tid;
    snprintf(s->contentDir, sizeof(s->contentDir), "%s/title/%08" PRIx32 "/%08" PRIx32 "/content", ctrRoot, hi, lo);
    snprintf(s->contentRel, sizeof(s->contentRel), "/title/%08" PRIx32 "/%08" PRIx32 "/content", hi, lo);

    // TMD installato: il file .tmd con il numero piu' alto
    DIR* d = opendir(s->contentDir);
    if(d == NULL) {
        seterr(s, "cartella content non trovata");
        return -1;
    }

    char tmdName[64] = {0};
    struct dirent* e;
    while((e = readdir(d)) != NULL) {
        if(ends_with(e->d_name, ".tmd") && strlen(e->d_name) < sizeof(tmdName) && strcmp(e->d_name, tmdName) > 0) {
            strcpy(tmdName, e->d_name);
        }
    }

    closedir(d);
    if(tmdName[0] == 0) {
        seterr(s, "nessun .tmd");
        return -2;
    }

    char path[800], rel[800];
    snprintf(path, sizeof(path), "%s/%s", s->contentDir, tmdName);
    snprintf(rel, sizeof(rel), "%s/%s", s->contentRel, tmdName);

    uint8_t* tmd = NULL;
    size_t tmdSize = 0;
    if(read_whole(path, &tmd, &tmdSize) != 0) {
        seterr(s, "lettura TMD fallita");
        return -3;
    }

    uint8_t ctr[16];
    sd_path_ctr(rel, ctr);
    aes_ctr_crypt(rk, ctr, 0, tmd, tmdSize);

    char cpName[64];
    uint8_t certs[CIA_CERT_SIZE];
    int rc = 0;
    if(cia_tmd_cp_name(tmd, tmdSize, cpName) != 0) {
        seterr(s, "TMD non valido (chiavi sbagliate?)");
        rc = -4;
    } else if(cia_find_certs(certsDb, certsDbSize, cpName, certs) != 0) {
        snprintf(s->err, sizeof(s->err), "certificati %s non trovati in certs.db", cpName);
        rc = -5;
    } else if(cia_front_build(&s->front, tmd, tmdSize, certs) != 0) {
        seterr(s, "costruzione CIA fallita");
        rc = -6;
    } else if(s->front.tid != tid) {
        seterr(s, "title ID del TMD diverso");
        rc = -7;
    }

    free(tmd);
    if(rc != 0) {
        return rc;
    }

    content_dirs_scan(&s->dirs, s->contentDir);
    for(int i = 0; i < s->front.n; i++) {
        int sub = -1;
        uint64_t sz = 0;
        if(content_locate(&s->dirs, s->contentDir, s->contentRel, s->front.c[i].id, path, sizeof(path), rel, sizeof(rel), &sub, &sz) != 0) {
            snprintf(s->err, sizeof(s->err), "content %08" PRIx32 " non installato", s->front.c[i].id);
            return -8;
        }

        if(sz != s->front.c[i].size) {
            snprintf(s->err, sizeof(s->err), "content %08" PRIx32 ": dimensione diversa dal TMD", s->front.c[i].id);
            return -9;
        }

        s->sub[i] = (int8_t) sub;
        sd_path_ctr(rel, s->ctr[i]);
    }

    return 0;
}

static FILE* content_file(cia_stream* s, int i) {
    if(s->fileIdx == i) {
        return s->file;
    }

    if(s->file != NULL) {
        fclose(s->file);
        s->file = NULL;
        s->fileIdx = -1;
    }

    char path[800], rel[800];
    content_paths(&s->dirs, s->contentDir, s->contentRel, s->front.c[i].id, s->sub[i], path, sizeof(path), rel, sizeof(rel));
    s->file = fopen(path, "rb");
    if(s->file != NULL) {
        setvbuf(s->file, NULL, _IONBF, 0);
        s->fileIdx = i;
    }

    return s->file;
}

#ifdef __3DS__
#define CIA_TICK() svcGetSystemTick()
#else
#define CIA_TICK() 0
#endif

int cia_stream_read(cia_stream* s, uint64_t off, uint8_t* buf, size_t len) {
    const cia_front* f = &s->front;
    if(off + len > f->total) {
        seterr(s, "lettura oltre la fine del CIA");
        return -1;
    }

    while(len > 0) {
        if(off < f->contentStart) {
            size_t n = len < f->contentStart - off ? len : (size_t) (f->contentStart - off);
            memcpy(buf, f->front + off, n);
            buf += n;
            off += n;
            len -= n;
            continue;
        }

        int ci = -1;
        for(int i = f->n - 1; i >= 0; i--) {
            if(off >= f->start[i]) {
                ci = i;
                break;
            }
        }

        uint64_t rel = off - f->start[ci];
        uint64_t aligned = (f->c[ci].size + 0x3F) & ~0x3Full;
        size_t n = len < aligned - rel ? len : (size_t) (aligned - rel);

        if(rel >= f->c[ci].size) {
            memset(buf, 0, n); // padding a 0x40
        } else {
            size_t real = n < f->c[ci].size - rel ? n : (size_t) (f->c[ci].size - rel);
            FILE* fp = content_file(s, ci);
            if(fp == NULL) {
                seterr(s, "apertura content fallita");
                return -2;
            }

            uint64_t k0 = CIA_TICK();
            size_t skew = (size_t) (rel & 15);
            uint8_t* tmp = buf;
            uint8_t* scratch = NULL;
            if(skew != 0) {
                scratch = malloc(real + skew);
                if(scratch == NULL) {
                    seterr(s, "memoria insufficiente");
                    return -3;
                }

                tmp = scratch;
            }

            if(fseeko(fp, (off_t) (rel - skew), SEEK_SET) != 0 || fread(tmp, 1, real + (scratch ? skew : 0), fp) != real + (scratch ? skew : 0)) {
                free(scratch);
                seterr(s, "lettura content fallita");
                return -4;
            }

            uint64_t k1 = CIA_TICK();
            aes_ctr_crypt(s->rk, s->ctr[ci], rel - skew, tmp, real + (scratch ? skew : 0));
            uint64_t k2 = CIA_TICK();
            s->tkFread += k1 - k0;
            s->tkAes += k2 - k1;
            s->bytesRead += real;

            if(scratch != NULL) {
                memcpy(buf, scratch + skew, real);
                free(scratch);
            }

            if(real < n) {
                memset(buf + real, 0, n - real);
            }
        }

        buf += n;
        off += n;
        len -= n;
    }

    return 0;
}

void cia_stream_close(cia_stream* s) {
    if(s->file != NULL) {
        fclose(s->file);
        s->file = NULL;
    }

    s->fileIdx = -1;
}
