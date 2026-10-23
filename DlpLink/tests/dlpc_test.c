// dlpc_test.c -- PC proof of the composer of the Download Play child (AzaharLink/ConfigApp/dlpc/dlpc.c, contract dlpc.h, design DlpLink/docs/COMPOSITORE.md).
// It composes from the REAL files (the app's CIA, the plugin, the models that DlpLink/tools/make_model.py makes from the finished children) and checks:
//   1  the composed children are byte for byte the ones makerom + finalize_child.py made (both variants), their RomFS holds the three files and the manifest,
//      the progress contract (stages in order, 0..total, no gap past 64 KB), memory and size announcements, determinism, a stale file replaced
//   2  the real child decrypted with the zero key is the emulator child except flags, access descriptor and the TMD hashes that follow
//   3  the independent tools: verify_cia.py (VALIDO, the same counts as for the reference), ctrtool (extracts the same three files)
//   4  the acceptors: dlph_open (name '3DS Link Installer', version, bytes, blocks; the emulator child refused) and the installer's own ParseCia/ParseManifest
//   5  a cancel at EVERY callback of a whole job (and none left behind)
//   6  errors: damaged/truncated/foreign models, versions, payloads that are not what they should be, read failures, sizes past the limit (including 2^63 and
//      UINT64_MAX), unwritable output, out of memory; never a file left behind
//   7  mutations: one flipped byte in the model (hundreds, every region), in the app's CIA and in the plugin (each region): refused, or a file that verify_cia.py
//      refuses against the original files, never a good-looking wrong file
//   8  the card lies: a short write, a full card, a byte flipped on the way to the card or on the way back (about a hundred places across the file), a failed rename
//   9  synthetic payloads (every size around the block boundaries, with and without meta), the 32 MiB limit to the byte, and the model rules against make_model.py
// Build and run: DlpLink/tests/run_checks.sh (step 6). Arguments: see usage().
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "aes.h"
#include "cia_front.h"
#include "dlpc.h"
#include "sha256.h"

// ---- the acceptors (dlpc_glue_dlph.c, dlpc_glue_inst.cpp)
int dlpc_glue_dlph(const char* path, char* name, size_t name_size, unsigned* version, unsigned long long* tid, unsigned long* child_bytes,
                   unsigned long* transfer_bytes, unsigned long* blocks);
int dlpc_glue_inst(const unsigned char* child, size_t child_n, const unsigned char* app, size_t app_n, const char* manifest, size_t manifest_n,
                   const unsigned char* plugin, size_t plugin_n, unsigned long long child_tid, unsigned long long app_tid, unsigned version16,
                   const char* version_text, char* msg, size_t msg_size);

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// checks
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
static int g_checks, g_failed;
static void check(int ok, const char* file, int line, const char* fmt, ...) {
    g_checks++;
    if (ok) {
        return;
    }
    g_failed++;
    printf("FAIL %s:%d: ", file, line);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}
#define CHECK(c, ...) check((c) != 0, __FILE__, __LINE__, __VA_ARGS__)

static struct {
    const char *work, *app, *plugin, *model, *model_emu, *ref, *ref_emu, *stage, *verify, *python, *ctrtool, *tools, *agree, *name, *desc, *author, *elf;
    const char* vtext;
    uint64_t tid;
    unsigned v16;
    int bench, quick;
} O = {.python = "python3", .vtext = "1.0.0", .tid = 0x000400000F83D100ull, .v16 = 1024};

static void usage(void) {
    printf("usage: dlpc_test --work DIR --app 3dslink.cia --plugin AzaharLink.3gx --model child_model.bin --model-emu child_model.emu.bin\n"
           "                 [--ref 3dslink-child.cia] [--ref-emu 3dslink-child.emu.cia] [--stage child_romfs_dir]\n"
           "                 [--verify verify_cia.py] [--python python3] [--ctrtool ctrtool] [--tools DlpLink/tools] [--agree dlpc_model_agree.py]\n"
           "                 [--expect NAME DESC AUTHOR] [--elf child.elf] [--tid HEX] [--version16 N] [--version-text 1.0.0] [--bench] [--quick]\n");
    exit(2);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// little helpers
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}
static uint64_t rd64(const uint8_t* p) {
    return (uint64_t) rd32(p) | ((uint64_t) rd32(p + 4) << 32);
}
static uint32_t rb32(const uint8_t* p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}
static void wb64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t) (v >> (56 - 8 * i));
    }
}
static void wr32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t) (v >> (8 * i));
    }
}
static void wr64(uint8_t* p, uint64_t v) {
    wr32(p, (uint32_t) v);
    wr32(p + 4, (uint32_t) (v >> 32));
}
static uint64_t al(uint64_t x, uint64_t a) {
    return (x + a - 1) & ~(a - 1);
}
static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec + (double) t.tv_nsec * 1e-9;
}
static char* P(const char* fmt, ...) { // a path or a command in one of 64 rotating buffers (a string that is kept across many calls goes in an array of its own)
    static char ring[64][2048];
    static int k;
    char* b = ring[k++ & 63];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(ring[0]), fmt, ap);
    va_end(ap);
    return b;
}
static uint8_t* load(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = (uint8_t*) malloc((size_t) sz + 1);
    if (!b || fread(b, 1, (size_t) sz, f) != (size_t) sz) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    *n = (size_t) sz;
    return b;
}
static void save(const char* path, const void* d, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f || fwrite(d, 1, n, f) != n || fclose(f) != 0) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
}
static int exists(const char* path) {
    return access(path, F_OK) == 0;
}
static int residue(const char* out) { // anything of the job left on the card?
    return exists(out) || exists(P("%s.part", out));
}
static uint32_t g_rng = 12345;
static uint32_t rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}
static void fill_random(uint8_t* p, size_t n, uint32_t seed) {
    uint32_t s = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        p[i] = (uint8_t) (s >> 24);
    }
}
static void hex32(const uint8_t h[32], char out[65]) {
    for (int i = 0; i < 32; i++) {
        snprintf(out + 2 * i, 3, "%02x", h[i]);
    }
}

// What a command prints (stdout and stderr together, the first 64 KB) and its exit status.
static char g_cmd_out[65536];
static int run_cmd(const char* cmd) {
    char full[4096];
    snprintf(full, sizeof(full), "%s 2>&1", cmd);
    FILE* p = popen(full, "r");
    size_t n = 0;
    if (p) {
        size_t got;
        while (n + 1 < sizeof(g_cmd_out) && (got = fread(g_cmd_out + n, 1, sizeof(g_cmd_out) - 1 - n, p)) > 0) {
            n += got;
        }
        char sink[512];
        while (fread(sink, 1, sizeof(sink), p) > 0) {
        }
    }
    g_cmd_out[n] = 0;
    int st = p ? pclose(p) : -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// fault injection (the test is linked with -Wl,--wrap=calloc,--wrap=fwrite,--wrap=fread,--wrap=rename: the composer's own calls go through these)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
enum { FM_OFF = 0, FM_SHORT = 1, FM_CORRUPT = 2 };
static struct {
    int calloc_fail_at, calloc_calls;
    size_t calloc_bytes;
    int fw_mode, fw_errno, fr_mode, rename_fail;
    uint64_t fw_at, fw_off, fr_at, fr_off;
} F;
void* __real_calloc(size_t n, size_t m);
void* __wrap_calloc(size_t n, size_t m) {
    F.calloc_calls++;
    if (F.calloc_fail_at != 0 && F.calloc_calls == F.calloc_fail_at) {
        return NULL;
    }
    F.calloc_bytes += n * m;
    return __real_calloc(n, m);
}
size_t __real_fwrite(const void* p, size_t sz, size_t n, FILE* f);
size_t __wrap_fwrite(const void* p, size_t sz, size_t n, FILE* f) {
    if (F.fw_mode == FM_OFF) {
        return __real_fwrite(p, sz, n, f);
    }
    const uint64_t len = (uint64_t) sz * n, start = F.fw_off;
    F.fw_off += len;
    if (F.fw_mode == FM_SHORT && start + len > F.fw_at) {
        const size_t ok = F.fw_at > start ? (size_t) (F.fw_at - start) : 0;
        __real_fwrite(p, 1, ok, f);
        errno = F.fw_errno;
        return sz ? ok / sz : 0;
    }
    if (F.fw_mode == FM_CORRUPT && F.fw_at >= start && F.fw_at < start + len) {
        uint8_t* t = (uint8_t*) malloc((size_t) len);
        memcpy(t, p, (size_t) len);
        t[F.fw_at - start] ^= 0x40;
        const size_t r = __real_fwrite(t, sz, n, f);
        free(t);
        return r;
    }
    return __real_fwrite(p, sz, n, f);
}
size_t __real_fread(void* p, size_t sz, size_t n, FILE* f);
size_t __wrap_fread(void* p, size_t sz, size_t n, FILE* f) {
    if (F.fr_mode == FM_OFF) {
        return __real_fread(p, sz, n, f);
    }
    const uint64_t start = F.fr_off;
    size_t got = __real_fread(p, sz, n, f);
    uint64_t len = (uint64_t) got * sz;
    if (F.fr_mode == FM_SHORT && start + len > F.fr_at) {
        got = F.fr_at > start && sz ? (size_t) ((F.fr_at - start) / sz) : 0;
        len = (uint64_t) got * sz;
    }
    if (F.fr_mode == FM_CORRUPT && F.fr_at >= start && F.fr_at < start + len) {
        ((uint8_t*) p)[F.fr_at - start] ^= 0x01;
    }
    F.fr_off += len;
    return got;
}
int __real_rename(const char* a, const char* b);
int __wrap_rename(const char* a, const char* b) {
    if (F.rename_fail) {
        errno = EIO;
        return -1;
    }
    return __real_rename(a, b);
}
static void faults_off(void) {
    memset(&F, 0, sizeof(F));
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// the payloads, the models, the jobs
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
typedef struct {
    const uint8_t* p;
    size_t n;
    int fail_call, calls; // the k-th read (1-based) fails; 0 = never
} src_t;
static int src_read(void* u, uint64_t off, uint8_t* buf, size_t len) {
    src_t* s = (src_t*) u;
    s->calls++;
    if (s->fail_call != 0 && s->calls == s->fail_call) {
        return 1;
    }
    if (off > s->n || len > s->n - (size_t) off) {
        return 1;
    }
    memcpy(buf, s->p + off, len);
    return 0;
}

static uint8_t *APP_D, *PLUG_D, *M_REAL, *M_EMU;
static size_t APP_N, PLUG_N, M_REAL_N, M_EMU_N;

typedef struct {
    int calls, cancel_at, bad;
    int last_stage, seen;
    uint64_t last_done, last_total, gap_max;
    double t0[8], t1[8];
    uint64_t total[8];
    char first_bad[160];
} prog_t;
static void prog_bad(prog_t* p, const char* msg) {
    if (p->bad++ == 0) {
        snprintf(p->first_bad, sizeof(p->first_bad), "%s", msg);
    }
}
static int prog_cb(void* u, int stage, uint64_t done, uint64_t total) {
    prog_t* p = (prog_t*) u;
    p->calls++;
    if (stage < 1 || stage > 6) {
        prog_bad(p, "stage out of range");
    } else {
        if (stage != p->last_stage) {
            if (stage != p->last_stage + 1) {
                prog_bad(p, "stages not in order");
            }
            if (p->last_stage && p->last_done != p->last_total) {
                prog_bad(p, "a stage did not end at done == total");
            }
            if (done != 0) {
                prog_bad(p, "a stage does not begin at 0");
            }
            p->last_done = 0;
            p->t0[stage] = now_s();
            p->total[stage] = total;
            p->seen |= 1 << stage;
        } else {
            if (total != p->last_total) {
                prog_bad(p, "total changed inside a stage");
            }
            if (done < p->last_done || done > total) {
                prog_bad(p, "done went back or past total");
            }
            if (done - p->last_done > p->gap_max) {
                p->gap_max = done - p->last_done;
            }
        }
        p->t1[stage] = now_s();
    }
    p->last_stage = stage;
    p->last_done = done;
    p->last_total = total;
    return p->cancel_at != 0 && p->calls == p->cancel_at;
}

static dlpc_job job_for(int emu, src_t* a, src_t* p, const char* out, prog_t* pr) {
    dlpc_job j;
    memset(&j, 0, sizeof(j));
    j.model = emu ? M_EMU : M_REAL;
    j.model_size = emu ? M_EMU_N : M_REAL_N;
    j.app_tid = O.tid;
    j.version16 = (uint16_t) O.v16;
    j.version_text = O.vtext;
    j.read_app = src_read;
    j.app_user = a;
    j.app_size = a->n;
    j.read_plugin = src_read;
    j.plugin_user = p;
    j.plugin_size = p->n;
    j.out_path = out;
    j.emu_variant = emu;
    j.progress = pr ? prog_cb : NULL;
    j.progress_user = pr;
    return j;
}
static char g_err[512];
static dlpc_result compose(const dlpc_job* j) {
    g_err[0] = 0;
    return dlpc_compose(j, g_err, sizeof(g_err));
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// an independent reader of a composed child: CIA -> NCCH -> RomFS (zero-key decryption when the NCCH says so) -> IVFC -> level 3 -> the three files
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
static const char* const NAMES[3] = {"3dslink.cia", "AzaharLink.3gx", "manifest.txt"};
typedef struct {
    uint8_t* region;
    size_t rsize;
    const uint8_t* file[3];
    size_t flen[3];
    char err[160];
} rfs_t;
static int rfs_fail(rfs_t* r, const char* m) {
    snprintf(r->err, sizeof(r->err), "%s", m);
    return -1;
}
static int rfs_extract(const uint8_t* cia, size_t n, rfs_t* r) {
    memset(r, 0, sizeof(*r));
    if (n < 0x3900) {
        return rfs_fail(r, "short file");
    }
    const size_t oc = (size_t) (al(0x2020, 64) + al(rd32(cia + 8), 64) + al(rd32(cia + 12), 64) + al(rd32(cia + 16), 64));
    if (oc + 0x200 > n) {
        return rfs_fail(r, "no NCCH");
    }
    const uint8_t* nc = cia + oc;
    const uint64_t roff = (uint64_t) rd32(nc + 0x1B0) * 0x200, rsz = (uint64_t) rd32(nc + 0x1B4) * 0x200;
    if (rsz < 0x2000 || oc + roff + rsz > n) {
        return rfs_fail(r, "RomFS outside the file");
    }
    r->rsize = (size_t) rsz;
    r->region = (uint8_t*) malloc(r->rsize);
    memcpy(r->region, nc + roff, r->rsize);
    if (!(nc[0x18F] & 4)) {
        uint32_t rk[44];
        uint8_t ctr[16], zero[16];
        memset(zero, 0, 16);
        memset(ctr, 0, 16);
        for (int i = 0; i < 8; i++) {
            ctr[i] = nc[0x10F - i]; // the partition id, big endian
        }
        ctr[8] = 3;
        aes_init();
        aes_set_key(rk, zero);
        aes_ctr_crypt(rk, ctr, 0, r->region, r->rsize);
    }
    const uint8_t* rg = r->region;
    if (memcmp(rg, "IVFC", 4) != 0 || rd32(rg + 4) != 0x10000) {
        return rfs_fail(r, "no IVFC");
    }
    const uint32_t mh = rd32(rg + 8), lg3 = rd32(rg + 0xC + 0x18 * 2 + 16);
    const uint64_t s3 = rd64(rg + 0xC + 0x18 * 2 + 8), base3 = al(0x60 + mh, (uint64_t) 1 << lg3);
    if (lg3 < 9 || lg3 > 20 || base3 + s3 > r->rsize) {
        return rfs_fail(r, "level 3 outside the RomFS");
    }
    const uint8_t* l3 = rg + base3;
    const uint32_t dm_o = rd32(l3 + 12), fm_o = rd32(l3 + 28), fm_s = rd32(l3 + 32), fd_o = rd32(l3 + 36);
    if (rd32(l3) != 0x28 || (uint64_t) fm_o + fm_s > s3 || fd_o > s3 || dm_o + 0x18 > s3) {
        return rfs_fail(r, "level 3 header");
    }
    uint32_t fo = rd32(l3 + dm_o + 12);
    int found = 0;
    for (int guard = 0; fo != 0xFFFFFFFFu && guard < 16; guard++) {
        if ((uint64_t) fo + 0x20 > fm_s) {
            return rfs_fail(r, "file entry outside its table");
        }
        const uint8_t* fe = l3 + fm_o + fo;
        const uint32_t nlen = rd32(fe + 0x1C);
        const uint64_t off = rd64(fe + 8), size = rd64(fe + 16);
        if (nlen > 60 || (uint64_t) fo + 0x20 + nlen > fm_s || fd_o + off + size > s3) {
            return rfs_fail(r, "file entry out of bounds");
        }
        char name[40];
        for (uint32_t i = 0; i < nlen / 2; i++) {
            name[i] = (char) fe[0x20 + 2 * i];
        }
        name[nlen / 2] = 0;
        for (int k = 0; k < 3; k++) {
            if (strcmp(name, NAMES[k]) == 0) {
                r->file[k] = l3 + fd_o + off;
                r->flen[k] = (size_t) size;
                found |= 1 << k;
            }
        }
        fo = rd32(fe + 4);
    }
    return found == 7 ? 0 : rfs_fail(r, "not all three files are in the RomFS");
}
static void rfs_free(rfs_t* r) {
    free(r->region);
    r->region = NULL;
}

// manifest.txt as make_dlp_child.sh writes it, computed here on its own
static size_t expect_manifest(char* out, size_t cap, const uint8_t* app, size_t an, const uint8_t* plug, size_t pn) {
    uint8_t h1[32], h2[32];
    char x1[65], x2[65];
    sha256(app, an, h1);
    sha256(plug, pn, h2);
    hex32(h1, x1);
    hex32(h2, x2);
    return (size_t) snprintf(out, cap, "version=%s\nversion16=%u\ntid=%016llX\n3dslink.cia=%zu %s\nAzaharLink.3gx=%zu %s\n", O.vtext, O.v16, (unsigned long long) O.tid, an, x1, pn,
                             x2);
}

// the file is a good child of this payload: the three files of its RomFS are the inputs and the manifest is what the files say
static int child_has(const uint8_t* d, size_t n, const uint8_t* app, size_t an, const uint8_t* plug, size_t pn, char* why, size_t why_n) {
    rfs_t r;
    if (rfs_extract(d, n, &r) != 0) {
        snprintf(why, why_n, "%s", r.err);
        rfs_free(&r);
        return 0;
    }
    char man[512];
    const size_t mn = expect_manifest(man, sizeof(man), app, an, plug, pn);
    int ok = 1;
    if (r.flen[0] != an || memcmp(r.file[0], app, an) != 0) {
        snprintf(why, why_n, "the app's CIA in the RomFS differs");
        ok = 0;
    } else if (r.flen[1] != pn || memcmp(r.file[1], plug, pn) != 0) {
        snprintf(why, why_n, "the plugin in the RomFS differs");
        ok = 0;
    } else if (r.flen[2] != mn || memcmp(r.file[2], man, mn) != 0) {
        snprintf(why, why_n, "the manifest differs");
        ok = 0;
    }
    rfs_free(&r);
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// verify_cia.py
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
typedef struct {
    int ran, valid, ok, fail, info;
} vres_t;
static vres_t run_verify(const char* cia, int emu, const char* romfs_dir, int with_strings) {
    vres_t v;
    memset(&v, 0, sizeof(v));
    if (!O.verify) {
        return v;
    }
    char cmd[3000];
    int n = snprintf(cmd, sizeof(cmd), "%s -I %s '%s' --kind child --tid %016llX --version %u", O.python, O.verify, cia,
                     (unsigned long long) ((O.tid & 0xFFFFFFFFull) | 0x0004000100000000ull), O.v16);
    if (emu) {
        n += snprintf(cmd + n, sizeof(cmd) - (size_t) n, " --emu");
    }
    if (romfs_dir) {
        n += snprintf(cmd + n, sizeof(cmd) - (size_t) n, " --romfs-dir '%s'", romfs_dir);
    }
    if (with_strings && O.name) {
        n += snprintf(cmd + n, sizeof(cmd) - (size_t) n, " --expect-strings '%s' '%s' '%s'", O.name, O.desc, O.author);
    }
    if (!emu && O.elf && exists(O.elf)) {
        n += snprintf(cmd + n, sizeof(cmd) - (size_t) n, " --elf '%s'", O.elf);
    }
    v.ran = 1;
    run_cmd(cmd);
    const char* s = strstr(g_cmd_out, "RIEPILOGO:");
    if (s) {
        sscanf(s, "RIEPILOGO: %d OK, %d FAIL, %d INFO", &v.ok, &v.fail, &v.info);
        v.valid = strstr(s, "NON VALIDO") == NULL && strstr(s, "VALIDO") != NULL;
    }
    return v;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// the composed children, kept for the tests that follow
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
static uint8_t* COMP[2];
static size_t COMP_N[2];
static char COMP_PATH[2][1024];

static void prog_expect_ok(const prog_t* p, const char* what) {
    CHECK(p->bad == 0, "%s: progress contract: %s", what, p->first_bad);
    CHECK(p->seen == 0x7E, "%s: stages seen 0x%X (want all six)", what, p->seen);
    CHECK(p->last_stage == 6 && p->last_done == p->last_total, "%s: the last call is not 'checking' at done == total", what);
    CHECK(p->gap_max <= 70000, "%s: a gap of %llu bytes between two progress calls (about every 64 KB)", what, (unsigned long long) p->gap_max);
}

static void t_reference(void) {
    printf("== 1. compose from the real files, both variants\n");
    for (int emu = 0; emu < 2; emu++) {
        const char* tag = emu ? "emulator" : "shipping";
        snprintf(COMP_PATH[emu], sizeof(COMP_PATH[emu]), "%s/composed%s.cia", O.work, emu ? ".emu" : "");
        const char* out = COMP_PATH[emu];
        src_t a = {APP_D, APP_N, 0, 0}, p = {PLUG_D, PLUG_N, 0, 0};
        prog_t pr;
        memset(&pr, 0, sizeof(pr));
        dlpc_job j = job_for(emu, &a, &p, out, &pr);
        faults_off();
        const dlpc_result r = compose(&j);
        CHECK(r == DLPC_OK, "%s: compose says %d: %s", tag, (int) r, g_err);
        const size_t mem_used = F.calloc_bytes;
        CHECK(mem_used <= dlpc_memory_needed(&j) && mem_used + 65536 >= dlpc_output_size(&j), "%s: memory taken %zu, announced %zu, file %llu", tag, mem_used,
              dlpc_memory_needed(&j), (unsigned long long) dlpc_output_size(&j));
        printf("   %s: memory taken %zu bytes (announced %zu, the file is %llu)\n", tag, mem_used, dlpc_memory_needed(&j), (unsigned long long) dlpc_output_size(&j));
        prog_expect_ok(&pr, tag);
        CHECK(!exists(P("%s.part", out)), "%s: .part left behind", tag);
        COMP[emu] = load(out, &COMP_N[emu]);
        const uint8_t* d = COMP[emu];
        const size_t n = COMP_N[emu];
        CHECK(n == dlpc_output_size(&j), "%s: file is %zu bytes, dlpc_output_size said %llu", tag, n, (unsigned long long) dlpc_output_size(&j));
        const char* ref = emu ? O.ref_emu : O.ref;
        if (ref) {
            size_t rn;
            uint8_t* rd = load(ref, &rn);
            CHECK(rn == n && memcmp(rd, d, n < rn ? n : rn) == 0, "%s: NOT byte for byte the child that makerom + finalize_child.py made (%s)", tag, ref);
            if (rn == n && memcmp(rd, d, n) == 0) {
                printf("   %s: byte for byte the same as %s (%zu bytes)\n", tag, ref, n);
            }
            free(rd);
        }
        char why[200];
        CHECK(child_has(d, n, APP_D, APP_N, PLUG_D, PLUG_N, why, sizeof(why)), "%s: %s", tag, why);
        if (O.stage) { // the files of the stage folder that make_dlp_child.sh gave makerom, manifest included
            rfs_t rf;
            CHECK(rfs_extract(d, n, &rf) == 0, "%s: %s", tag, rf.err);
            for (int k = 0; k < 3 && rf.region; k++) {
                size_t sn;
                uint8_t* sd = load(P("%s/%s", O.stage, NAMES[k]), &sn);
                CHECK(sn == rf.flen[k] && memcmp(sd, rf.file[k], sn < rf.flen[k] ? sn : rf.flen[k]) == 0, "%s: %s differs from the stage folder's", tag, NAMES[k]);
                free(sd);
            }
            rfs_free(&rf);
        }
        // the same job twice gives the same bytes, and a stale file at the output is replaced
        save(P("%s.stale", out), "stale", 5);
        save(out, "an old child", 12);
        faults_off();
        src_t a2 = {APP_D, APP_N, 0, 0}, p2 = {PLUG_D, PLUG_N, 0, 0};
        dlpc_job j2 = job_for(emu, &a2, &p2, out, NULL);
        CHECK(compose(&j2) == DLPC_OK, "%s: second compose: %s", tag, g_err);
        size_t n2;
        uint8_t* d2 = load(out, &n2);
        CHECK(n2 == n && memcmp(d2, d, n) == 0, "%s: two runs of the same job differ", tag);
        free(d2);
        remove(P("%s.stale", out));
        // the announcements
        CHECK(dlpc_model_version(emu ? M_EMU : M_REAL, emu ? M_EMU_N : M_REAL_N) == O.v16, "dlpc_model_version of the %s model", tag);
        for (int s = 1; s <= 6; s++) {
            const dlpc_work w = dlpc_stage_work(&j, s);
            CHECK(w.sd_read + w.sd_write + w.sha + w.aes > 0, "%s: stage %d has no work announced", tag, s);
            CHECK((w.aes != 0) == (!emu && (s == DLPC_STAGE_SEAL || s == DLPC_STAGE_VERIFY)), "%s: stage %d: aes %llu", tag, s, (unsigned long long) w.aes);
            CHECK(dlpc_stage_text(s)[0] != 0, "stage %d has no text", s);
        }
        CHECK(dlpc_stage_work(&j, 0).sha == 0 && dlpc_stage_work(&j, 7).sha == 0, "work for a stage that does not exist");
    }
    CHECK(dlpc_model_version(M_REAL, M_REAL_N - 1) == 0 && dlpc_model_version(NULL, 0) == 0 && dlpc_model_version(M_REAL, 10) == 0, "dlpc_model_version of what is not a model");
    for (int r = DLPC_OK; r <= DLPC_E_CANCEL; r++) {
        CHECK(dlpc_result_text((dlpc_result) r)[0] != 0, "result %d has no text", r);
    }
}

static void t_relation(void) {
    printf("== 2. the shipping child, decrypted with the zero key, is the emulator child but for flags, access descriptor and the TMD hashes\n");
    if (!COMP[0] || !COMP[1] || COMP_N[0] != COMP_N[1]) {
        CHECK(0, "both composed children are needed");
        return;
    }
    const size_t n = COMP_N[0];
    uint8_t* real = (uint8_t*) malloc(n);
    memcpy(real, COMP[0], n);
    const uint8_t* emu = COMP[1];
    const size_t oc = (size_t) (al(0x2020, 64) + al(rd32(real + 8), 64) + al(rd32(real + 12), 64) + al(rd32(real + 16), 64));
    const size_t om = oc - (size_t) al(rd32(real + 16), 64);
    const size_t hb = om + 4 + 0x100 + 0x3C; // the TMD of the model is RSA-2048 SHA-256 (0x10004)
    CHECK(rb32(real + om) == 0x10004, "TMD signature type %X", rb32(real + om));
    uint8_t* nc = real + oc;
    uint32_t rk[44];
    uint8_t zero[16], ctr[16];
    memset(zero, 0, 16);
    aes_init();
    aes_set_key(rk, zero);
    const struct {
        size_t off, size;
        uint8_t sec;
    } secs[3] = {{0x200, 0x800, 1}, {(size_t) rd32(nc + 0x1A0) * 0x200, (size_t) rd32(nc + 0x1A4) * 0x200, 2}, {(size_t) rd32(nc + 0x1B0) * 0x200, (size_t) rd32(nc + 0x1B4) * 0x200, 3}};
    for (int s = 0; s < 3; s++) {
        memset(ctr, 0, 16);
        wb64(ctr, rd64(nc + 0x108));
        ctr[8] = secs[s].sec;
        aes_ctr_crypt(rk, ctr, 0, nc + secs[s].off, secs[s].size);
    }
    // the bytes that may differ: the flags byte, the access descriptor (the second half of the exheader), the TMD hashes and the content id
    const size_t cr = hb + 0x9C4, info = hb + 0xC4;
    const struct {
        size_t a, b;
        const char* what;
    } allowed[] = {{oc + 0x18F, oc + 0x190, "NCCH flags"}, {oc + 0x200 + 0x400, oc + 0x200 + 0x800, "access descriptor"}, {cr, cr + 4, "content id"},
                   {cr + 0x10, cr + 0x30, "content hash"}, {info + 4, info + 0x24, "info record hash"}, {hb + 0xA4, hb + 0xC4, "TMD header hash"}};
    size_t diff[7] = {0}, stray = 0, first_stray = 0;
    for (size_t i = 0; i < n; i++) {
        if (real[i] == emu[i]) {
            continue;
        }
        int k = -1;
        for (int q = 0; q < 6; q++) {
            if (i >= allowed[q].a && i < allowed[q].b) {
                k = q;
            }
        }
        if (k < 0) {
            if (stray++ == 0) {
                first_stray = i;
            }
        } else {
            diff[k]++;
        }
    }
    CHECK(stray == 0, "%zu bytes differ outside the allowed places (the first at 0x%zX)", stray, first_stray);
    CHECK(diff[0] == 1 && diff[1] > 0 && diff[3] > 0 && diff[4] > 0 && diff[5] > 0, "the expected differences are missing (flags %zu, descriptor %zu, hashes %zu/%zu/%zu)", diff[0], diff[1], diff[3], diff[4], diff[5]);
    printf("   differences: flags %zu, descriptor %zu, content id %zu, hashes %zu+%zu+%zu bytes; everywhere else the files are the same (%zu bytes)\n", diff[0], diff[1], diff[2], diff[3],
           diff[4], diff[5], n);
    free(real);
}

static void t_external(void) {
    printf("== 3. the independent tools: verify_cia.py and ctrtool\n");
    if (!O.verify) {
        printf("   (no --verify: skipped)\n");
        return;
    }
    vres_t ref[2];
    memset(ref, 0, sizeof(ref));
    for (int emu = 0; emu < 2; emu++) {
        const char* r = emu ? O.ref_emu : O.ref;
        if (r) {
            ref[emu] = run_verify(r, emu, O.stage, 1);
        }
        const vres_t v = run_verify(COMP_PATH[emu], emu, O.stage, 1);
        CHECK(v.ran && v.valid && v.fail == 0, "verify_cia.py on the composed %s child: %d OK, %d FAIL%s", emu ? "emulator" : "shipping", v.ok, v.fail, v.valid ? "" : " (NOT VALID)");
        if (r) {
            CHECK(v.ok == ref[emu].ok && v.fail == ref[emu].fail && v.info == ref[emu].info, "verify_cia.py counts: composed %d/%d/%d, reference %d/%d/%d", v.ok, v.fail, v.info,
                  ref[emu].ok, ref[emu].fail, ref[emu].info);
        }
        printf("   verify_cia.py on the composed %s child: %d OK, %d FAIL, %d INFO -- %s\n", emu ? "emulator" : "shipping", v.ok, v.fail, v.info, v.valid ? "VALIDO" : "NOT VALID");
    }
    if (O.ctrtool && exists(O.ctrtool)) {
        for (int emu = 0; emu < 2; emu++) {
            char dir[1024]; snprintf(dir, sizeof(dir), "%s/ctr%d", O.work, emu);
            run_cmd(P("rm -rf '%s' && mkdir -p '%s'", dir, dir));
            run_cmd(P("'%s' -t cia --contents='%s/c' '%s'", O.ctrtool, dir, COMP_PATH[emu]));
            // (the content is named by its content id: 0 in the shipping child, makerom's own id in the emulator one)
            const int rc = run_cmd(P("f=$(ls '%s'/c.0000.* | head -1) && '%s' -t ncch --romfsdir='%s/rf' --exefsdir='%s/ex' \"$f\"", dir, O.ctrtool, dir, dir));
            CHECK(rc == 0, "ctrtool could not read the composed %s child (%d)", emu ? "emulator" : "shipping", rc);
            const uint8_t* want[3] = {APP_D, PLUG_D, NULL};
            const size_t wn[3] = {APP_N, PLUG_N, 0};
            for (int k = 0; k < 3; k++) {
                size_t gn = 0;
                uint8_t* g = exists(P("%s/rf/%s", dir, NAMES[k])) ? load(P("%s/rf/%s", dir, NAMES[k]), &gn) : NULL;
                if (k < 2) {
                    CHECK(g && gn == wn[k] && memcmp(g, want[k], gn) == 0, "ctrtool: %s of the composed %s child differs from the input", NAMES[k], emu ? "emulator" : "shipping");
                } else {
                    char man[512];
                    const size_t mn = expect_manifest(man, sizeof(man), APP_D, APP_N, PLUG_D, PLUG_N);
                    CHECK(g && gn == mn && memcmp(g, man, mn) == 0, "ctrtool: manifest.txt of the composed %s child differs", emu ? "emulator" : "shipping");
                }
                free(g);
            }
            printf("   ctrtool extracts the three files of the composed %s child, identical to the inputs\n", emu ? "emulator" : "shipping");
        }
    } else {
        printf("   (no ctrtool: skipped)\n");
    }
}

static void t_acceptors(void) {
    printf("== 4. the acceptors: dlph_open (Download Play host library) and the installer's own reading code\n");
    char name[80], rname[80], msg[200];
    unsigned ver = 0, rver = 0;
    unsigned long long tid = 0, rtid = 0;
    unsigned long cb = 0, tb = 0, bl = 0, rcb = 0, rtb = 0, rbl = 0;
    const int ok = dlpc_glue_dlph(COMP_PATH[0], name, sizeof(name), &ver, &tid, &cb, &tb, &bl);
    CHECK(ok, "dlph_open refuses the composed shipping child");
    CHECK(strcmp(name, O.name ? O.name : "3DS Link Installer") == 0 && ver == O.v16 && tid == (0x0004000100000000ull | (O.tid & 0xFFFFFFFFull)), "dlph announces '%s' v%u tid %llX", name,
          ver, tid);
    CHECK(cb == COMP_N[0] && tb > 0 && tb < cb && bl >= 1, "dlph: %lu bytes, %lu to download, %lu blocks", cb, tb, bl);
    printf("   dlph_open accepts the shipping child: '%s', version %u, %lu bytes (%lu to download, %lu blocks of 182 x 1440)\n", name, ver, cb, tb, bl);
    if (O.ref) {
        const int rok = dlpc_glue_dlph(O.ref, rname, sizeof(rname), &rver, &rtid, &rcb, &rtb, &rbl);
        CHECK(rok && strcmp(name, rname) == 0 && ver == rver && tid == rtid && cb == rcb && tb == rtb && bl == rbl, "dlph on the reference child says something else (%s v%u %lu/%lu/%lu)", rname, rver,
              rcb, rtb, rbl);
    }
    CHECK(!dlpc_glue_dlph(COMP_PATH[1], name, sizeof(name), &ver, &tid, &cb, &tb, &bl), "dlph_open must refuse the emulator child (it would be offered to real consoles)");
    printf("   dlph_open refuses the emulator child\n");
    for (int emu = 0; emu < 2; emu++) {
        rfs_t r;
        CHECK(rfs_extract(COMP[emu], COMP_N[emu], &r) == 0, "%s", r.err);
        if (!r.region) {
            continue;
        }
        const int bad = dlpc_glue_inst(COMP[emu], COMP_N[emu], r.file[0], r.flen[0], (const char*) r.file[2], r.flen[2], r.file[1], r.flen[1], 0x0004000100000000ull | (O.tid & 0xFFFFFFFFull), O.tid,
                                       O.v16, O.vtext, msg, sizeof(msg));
        CHECK(bad == 0, "the installer's reading code on the composed %s child: %d failed, the first: %s", emu ? "emulator" : "shipping", bad, msg);
        rfs_free(&r);
    }
    printf("   the installer's ParseCia / ParseManifest / Decide read the composed children, their app and their manifest as they should\n");
}

static dlpc_result try_job(dlpc_job j, const char* what, dlpc_result want);

// The app's CIA as the sender of Titles Delivery makes it from the installed title (titleshare/common/cia_front.c, source (b) of COMPOSITORE.md): the TMD of
// the title, a generic ticket, no meta block. The composer must take it like the file that the installer leaves (source (a)).
static void t_virtual(void) {
    printf("== 4b. the app's CIA as Titles Delivery's virtual CIA makes it (no meta, generic ticket)\n");
    cia_front* f = (cia_front*) malloc(sizeof(*f));
    uint8_t certs[CIA_CERT_SIZE];
    memcpy(certs, APP_D + 0x2040, sizeof(certs));
    const int rc = cia_front_build(f, APP_D + 0x2DC0, rd32(APP_D + 16), certs);
    CHECK(rc == 0 && f->frontSize == 0x3900 && f->total == f->frontSize + al(rd32(APP_D + 0x18), 64), "cia_front_build: %d, front 0x%zX, total %llu", rc, f->frontSize, (unsigned long long) f->total);
    const size_t vn = (size_t) f->total;
    uint8_t* v = (uint8_t*) calloc(1, vn);
    memcpy(v, f->front, f->frontSize);
    memcpy(v + f->frontSize, APP_D + 0x3900, vn - f->frontSize);
    CHECK(rd32(v + 0x14) == 0 && vn < APP_N, "the virtual CIA has no meta (%zu bytes against %zu)", vn, APP_N);
    char out[1024];
    snprintf(out, sizeof(out), "%s/virtual.cia", O.work);
    for (int emu = 0; emu < 2; emu++) {
        src_t a = {v, vn, 0, 0}, p = {PLUG_D, PLUG_N, 0, 0};
        dlpc_job j = job_for(emu, &a, &p, out, NULL);
        faults_off();
        const dlpc_result r = compose(&j);
        CHECK(r == DLPC_OK, "the virtual CIA as the app (%s): %s", emu ? "emulator" : "shipping", g_err);
        if (r != DLPC_OK) {
            continue;
        }
        size_t n;
        uint8_t* d = load(out, &n);
        char why[200], msg[200];
        CHECK(child_has(d, n, v, vn, PLUG_D, PLUG_N, why, sizeof(why)), "%s", why);
        rfs_t rf;
        if (rfs_extract(d, n, &rf) == 0) {
            CHECK(dlpc_glue_inst(d, n, rf.file[0], rf.flen[0], (const char*) rf.file[2], rf.flen[2], rf.file[1], rf.flen[1], 0x0004000100000000ull | (O.tid & 0xFFFFFFFFull), O.tid, O.v16, O.vtext, msg,
                                 sizeof(msg)) == 0,
                  "the installer's reading code on a child that carries the virtual CIA: %s", msg);
        }
        rfs_free(&rf);
        free(d);
        remove(out);
    }
    // the content ends at 0x9EA00, a multiple of 64: five bytes cut off the file are five bytes cut off the content itself, which the hash chain refuses
    {
        src_t a = {v, vn - 5, 0, 0}, p = {PLUG_D, PLUG_N, 0, 0};
        dlpc_job j = job_for(0, &a, &p, out, NULL);
        try_job(j, "the virtual CIA with 5 bytes cut off its content", DLPC_E_APP_BAD);
    }
    printf("   the virtual CIA (%zu bytes) is composed into a good child, both variants, and read by the installer's code\n", vn);
    free(v);
    free(f);
}

static void t_cancel(void) {
    printf("== 5. a cancel at every callback of a whole job leaves nothing\n");
    for (int emu = 0; emu < 2; emu++) {
        char out[1024]; snprintf(out, sizeof(out), "%s/cancel.cia", O.work);
        src_t a = {APP_D, APP_N, 0, 0}, p = {PLUG_D, PLUG_N, 0, 0};
        prog_t pr;
        memset(&pr, 0, sizeof(pr));
        dlpc_job j = job_for(emu, &a, &p, out, &pr);
        faults_off();
        CHECK(compose(&j) == DLPC_OK, "reference run: %s", g_err);
        const int total = pr.calls;
        remove(out);
        const int stride = emu ? (O.quick ? 9 : 3) : (O.quick ? 4 : 1);
        int cancelled = 0, per_stage[8] = {0};
        for (int k = 1; k <= total; k += stride) {
            src_t a2 = {APP_D, APP_N, 0, 0}, p2 = {PLUG_D, PLUG_N, 0, 0};
            prog_t q;
            memset(&q, 0, sizeof(q));
            q.cancel_at = k;
            dlpc_job j2 = job_for(emu, &a2, &p2, out, &q);
            save(out, "stale", 5);
            const dlpc_result r = compose(&j2);
            CHECK(r == DLPC_E_CANCEL, "cancel at call %d of %d (stage %d): result %d (%s)", k, total, q.last_stage, (int) r, g_err);
            CHECK(q.calls == k, "cancel at call %d: the callback was called %d times (nothing may be called after a cancel)", k, q.calls);
            CHECK(!residue(out), "cancel at call %d (stage %d): a file is left behind", k, q.last_stage);
            CHECK(g_err[0] != 0, "cancel at call %d: no sentence", k);
            if (r == DLPC_E_CANCEL) {
                cancelled++;
                per_stage[q.last_stage < 8 ? q.last_stage : 0]++;
            }
        }
        printf("   %s: %d callbacks in a job, cancelled at %d of them (stages 1..6: %d %d %d %d %d %d), nothing left behind each time\n", emu ? "emulator" : "shipping", total, cancelled,
               per_stage[1], per_stage[2], per_stage[3], per_stage[4], per_stage[5], per_stage[6]);
        for (int s = 1; s <= 6; s++) {
            CHECK(per_stage[s] > 0, "no cancel was tried in stage %d", s);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// errors
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
static dlpc_result try_job(dlpc_job j, const char* what, dlpc_result want) {
    faults_off();
    const dlpc_result r = compose(&j);
    CHECK(r == want, "%s: result %d (%s), wanted %d", what, (int) r, g_err, (int) want);
    CHECK(want == DLPC_OK || !residue(j.out_path ? j.out_path : "/nonexistent/x"), "%s: a file is left behind", what);
    CHECK(r == DLPC_OK || g_err[0] != 0, "%s: no sentence for the failure", what);
    return r;
}

static void t_errors(void) {
    printf("== 6. errors: a refusal says why and leaves nothing\n");
    char out[1024]; snprintf(out, sizeof(out), "%s/err.cia", O.work);
    src_t a = {APP_D, APP_N, 0, 0}, p = {PLUG_D, PLUG_N, 0, 0};
    dlpc_job base = job_for(0, &a, &p, out, NULL);

    // --- the model
    {
        dlpc_job j = base;
        j.model = NULL;
        j.model_size = 0;
        try_job(j, "no model", DLPC_E_MODEL);
        j = base;
        j.model_size = 0;
        try_job(j, "model of 0 bytes", DLPC_E_MODEL);
        const size_t cuts[] = {1, 0x7F, 0x80, 0x81, 0x1000, M_REAL_N / 2, M_REAL_N - 1};
        for (size_t c = 0; c < sizeof(cuts) / sizeof(cuts[0]); c++) {
            j = base;
            j.model_size = cuts[c];
            try_job(j, P("model cut to %zu bytes", cuts[c]), DLPC_E_MODEL);
        }
        uint8_t* big = (uint8_t*) malloc(M_REAL_N + 64);
        memcpy(big, M_REAL, M_REAL_N);
        memset(big + M_REAL_N, 0, 64);
        j = base;
        j.model = big;
        j.model_size = M_REAL_N + 64;
        try_job(j, "model with 64 bytes more", DLPC_E_MODEL);
        free(big);
        j = base;
        j.model = M_EMU;
        j.model_size = M_EMU_N;
        try_job(j, "the emulator model for a shipping child", DLPC_E_MODEL);
        j = job_for(1, &a, &p, out, NULL);
        j.model = M_REAL;
        j.model_size = M_REAL_N;
        try_job(j, "the shipping model for an emulator child", DLPC_E_MODEL);
        j = base;
        j.model = APP_D;
        j.model_size = APP_N;
        try_job(j, "the app's CIA as the model", DLPC_E_MODEL);
        j = base;
        j.read_app = NULL;
        try_job(j, "no read_app", DLPC_E_MODEL);
        j = base;
        j.out_path = NULL;
        faults_off();
        CHECK(compose(&j) == DLPC_E_MODEL, "no output path");
        CHECK(dlpc_compose(NULL, g_err, sizeof(g_err)) == DLPC_E_MODEL, "no job at all");
        CHECK(dlpc_compose(&base, NULL, 0) == DLPC_OK && exists(out), "no err buffer is fine");
        remove(out);
    }
    // --- versions
    {
        dlpc_job j = base;
        j.version16 = (uint16_t) (O.v16 + 1);
        try_job(j, "version16 not that of the model", DLPC_E_VERSION);
        {   // a version TEXT that is another patch than the model's (the model says O.v16: the text of the version before or after it, never a number written in the test: it was "1.0.1",
            // which was another version only while 3DS Link was 1.0.0)
            char other_text[32];
            const unsigned v = O.v16 ^ 1u;
            snprintf(other_text, sizeof(other_text), "%u.%u.%u", v >> 10, (v >> 4) & 63, v & 15);
            j = base;
            j.version_text = other_text;
            try_job(j, "version text that says another version", DLPC_E_VERSION);
        }
        {   // numbers that agree with each other but not with the model: the app and its model come from different builds
            const unsigned other = O.v16 ^ 1u;
            char text[32];
            snprintf(text, sizeof(text), "%u.%u.%u", other >> 10, (other >> 4) & 63, other & 15);
            j = base;
            j.version16 = (uint16_t) other;
            j.version_text = text;
            try_job(j, "a version (number and text) that is not the model's", DLPC_E_VERSION);
            CHECK(strstr(g_err, "another version") != NULL, "sentence of a model of another version: '%s'", g_err);
        }
        const char* bad_text[] = {"1.0", "1", "", "1.0.0.0", "1.0.0\nx=1", "1.0.0 ", "a.b.c", "01.0.0x", "1..0", ".1.0", "100.0.0", "1.0.0\r\n", "1.0.0;", "+1.0.0"};
        for (size_t i = 0; i < sizeof(bad_text) / sizeof(bad_text[0]); i++) {
            j = base;
            j.version_text = bad_text[i];
            try_job(j, P("version text '%s'", bad_text[i]), DLPC_E_VERSION);
        }
        j = base;
        j.version_text = NULL;
        try_job(j, "no version text", DLPC_E_MODEL);
        // the app of another version / another title (the TMD says so, the rest is untouched: the refusal comes before the hash chain)
        uint8_t* t = (uint8_t*) malloc(APP_N);
        memcpy(t, APP_D, APP_N);
        const size_t om = (size_t) (al(0x2020, 64) + al(rd32(t + 8), 64) + al(rd32(t + 12), 64)), hb = om + 4 + 0x100 + 0x3C;
        t[hb + 0x9D] ^= 1;
        src_t ta = {t, APP_N, 0, 0};
        j = job_for(0, &ta, &p, out, NULL);
        try_job(j, "an app of another version", DLPC_E_APP_BAD);
        t[hb + 0x9D] ^= 1;
        t[hb + 0x4F] ^= 1;
        try_job(j, "an app of another title", DLPC_E_APP_BAD);
        free(t);
        j = base;
        j.app_tid = 0;
        try_job(j, "no title for the app", DLPC_E_APP_BAD);
        j = base;
        j.app_tid = O.tid + 1;
        try_job(j, "another title than the app's CIA", DLPC_E_APP_BAD);
    }
    // --- payloads that are not what they should be
    {
        uint8_t* z = (uint8_t*) calloc(1, 2000000);
        fill_random(z + 1000000, 1000000, 7);
        const struct {
            const char* what;
            const uint8_t* d;
            size_t n;
        } bad_apps[] = {{"zeros as the app", z, 700000}, {"noise as the app", z + 1000000, 700000}, {"the plugin as the app", PLUG_D, PLUG_N}, {"the model as the app", M_REAL, M_REAL_N},
                        {"the app cut by 100 bytes", APP_D, APP_N - 100}, {"the app cut by 1", APP_D, APP_N - 1}, {"the app cut to half", APP_D, APP_N / 2},
                        {"the app cut to 0x3000", APP_D, 0x3000}, {"the app cut to 0x2040", APP_D, 0x2040}, {"the app cut to 0x203F", APP_D, 0x203F}, {"the app with a byte more", APP_D, APP_N}};
        for (size_t i = 0; i < sizeof(bad_apps) / sizeof(bad_apps[0]); i++) {
            uint8_t* t = (uint8_t*) malloc(bad_apps[i].n + 1);
            memcpy(t, bad_apps[i].d, bad_apps[i].n);
            t[bad_apps[i].n] = 0x99;
            src_t ta = {t, bad_apps[i].n + (strstr(bad_apps[i].what, "byte more") ? 1 : 0), 0, 0};
            dlpc_job j = job_for(0, &ta, &p, out, NULL);
            try_job(j, bad_apps[i].what, DLPC_E_APP_BAD);
            free(t);
        }
        const struct {
            const char* what;
            const uint8_t* d;
            size_t n;
        } bad_plugs[] = {{"zeros as the plugin", z, 100000}, {"noise as the plugin", z + 1000000, 100000}, {"the app as the plugin", APP_D, APP_N}, {"a plugin of 15 bytes", PLUG_D, 15},
                         {"a plugin of 0 bytes", PLUG_D, 0}, {"the model as the plugin", M_REAL, M_REAL_N}};
        for (size_t i = 0; i < sizeof(bad_plugs) / sizeof(bad_plugs[0]); i++) {
            src_t tp = {bad_plugs[i].d, bad_plugs[i].n, 0, 0};
            dlpc_job j = job_for(0, &a, &tp, out, NULL);
            try_job(j, bad_plugs[i].what, DLPC_E_PLUGIN_BAD);
        }
        // the plugin of another version: the bytes the app compares (8..11 of the header), and the ones it does not
        for (int pos = 8; pos <= 11; pos++) {
            uint8_t* t = (uint8_t*) malloc(PLUG_N);
            memcpy(t, PLUG_D, PLUG_N);
            t[pos] ^= 1;
            src_t tp = {t, PLUG_N, 0, 0};
            dlpc_job j = job_for(0, &a, &tp, out, NULL);
            if (pos == 8) {
                try_job(j, "byte 8 of the plugin header (not part of the version)", DLPC_OK);
                remove(out);
            } else {
                try_job(j, P("plugin of another version (byte %d)", pos), DLPC_E_PLUGIN_BAD);
            }
            free(t);
        }
        free(z);
        // read failures: at the first, a middle and the last read of each file
        for (int which = 0; which < 2; which++) {
            const size_t n = which ? PLUG_N : APP_N;
            const int reads = (int) ((n + 65535) / 65536);
            const int at[] = {1, reads / 2 + 1, reads};
            for (int i = 0; i < 3; i++) {
                src_t ta = {APP_D, APP_N, which ? 0 : at[i], 0}, tp = {PLUG_D, PLUG_N, which ? at[i] : 0, 0};
                dlpc_job j = job_for(0, &ta, &tp, out, NULL);
                try_job(j, P("read %d of the %s fails", at[i], which ? "plugin" : "app"), which ? DLPC_E_READ_PLUGIN : DLPC_E_READ_APP);
            }
        }
        // the file is shorter than its size says (the card lost the end): the read fails
        {
            src_t ta = {APP_D, APP_N - 5000, 0, 0};
            dlpc_job j = job_for(0, &ta, &p, out, NULL);
            j.app_size = APP_N;
            try_job(j, "the app file is shorter than its size", DLPC_E_READ_APP);
        }
    }
    // --- sizes: the limit is 32 MiB for each input and for the child; nothing is read, nothing wraps
    {
        const uint64_t big[] = {0x02000001ull, 0x04000000ull, 0x7FFFFFFFull, 0x80000000ull, 0xFFFFFFFFull, 0x100000000ull, 0x100000010ull, 1ull << 40, 1ull << 63, 0xFFFFFFFFFFFFFFFFull,
                                0xFFFFFFFFFFFFFFF0ull};
        for (size_t i = 0; i < sizeof(big) / sizeof(big[0]); i++) {
            dlpc_job j = base;
            j.app_size = big[i];
            try_job(j, P("app_size %llu", (unsigned long long) big[i]), DLPC_E_TOOBIG);
            CHECK(dlpc_output_size(&j) > 0x02000000ull, "output size of app_size %llu", (unsigned long long) big[i]);
            j = base;
            j.plugin_size = big[i];
            try_job(j, P("plugin_size %llu", (unsigned long long) big[i]), DLPC_E_TOOBIG);
            CHECK(dlpc_output_size(&j) > 0x02000000ull, "output size of plugin_size %llu", (unsigned long long) big[i]);
            CHECK(dlpc_memory_needed(&j) > 0x02000000ull, "memory needed for plugin_size %llu", (unsigned long long) big[i]);
        }
        dlpc_job j = base;
        j.app_size = 0x01400000ull;
        j.plugin_size = 0x01400000ull; // 20 MiB + 20 MiB: each is fine, the child is not
        try_job(j, "two payloads of 20 MiB", DLPC_E_TOOBIG);
        j = base;
        j.app_size = 0;
        try_job(j, "app of 0 bytes", DLPC_E_APP_BAD);
        j = base;
        j.plugin_size = 0;
        try_job(j, "plugin of 0 bytes", DLPC_E_PLUGIN_BAD);
        j = base;
        j.model = NULL;
        CHECK(dlpc_output_size(&j) == 0 && dlpc_memory_needed(&j) == 0, "sizes of a job without a model");
        CHECK(dlpc_output_size(NULL) == 0 && dlpc_memory_needed(NULL) == 0 && dlpc_stage_work(NULL, 1).sha == 0, "sizes of no job");
    }
    // --- the output
    {
        dlpc_job j = base;
        j.out_path = P("%s/no_such_folder/child.cia", O.work);
        try_job(j, "a folder that is not there", DLPC_E_WRITE);
        run_cmd(P("rm -rf '%s/adir.cia' && mkdir -p '%s/adir.cia' && echo x > '%s/adir.cia/keep'", O.work, O.work, O.work));
        j.out_path = P("%s/adir.cia", O.work);
        faults_off();
        CHECK(compose(&j) == DLPC_E_WRITE && !exists(P("%s/adir.cia.part", O.work)) && exists(P("%s/adir.cia/keep", O.work)), "a directory in the place of the child: %s", g_err);
        run_cmd(P("rm -rf '%s/adir.cia'", O.work));
        char longp[1200];
        memset(longp, 'x', sizeof(longp));
        memcpy(longp, O.work, strlen(O.work));
        longp[1100] = 0;
        j.out_path = longp;
        try_job(j, "a path of 1100 characters", DLPC_E_WRITE);
        j = base;
        j.out_path = "";
        try_job(j, "an empty path", DLPC_E_MODEL);
        // a stale child at the output is removed even when the job fails
        save(out, "an old child", 12);
        j = base;
        j.version16 = 77;
        faults_off();
        CHECK(compose(&j) == DLPC_E_VERSION && !exists(out), "a stale child must not survive a failed job");
        // the sentences
        j = base;
        j.version16 = 77;
        try_job(j, "sentence of E_VERSION", DLPC_E_VERSION);
        CHECK(strstr(g_err, "version") != NULL, "E_VERSION sentence: '%s'", g_err);
    }
    // --- out of memory (the composer's two allocations in turn)
    for (int at = 1; at <= 2; at++) {
        faults_off();
        F.calloc_fail_at = at;
        const dlpc_result r = compose(&base);
        CHECK(r == DLPC_E_NOMEM && !residue(out) && strstr(g_err, "memory") != NULL, "allocation %d refused: result %d '%s'", at, (int) r, g_err);
        faults_off();
    }
    printf("   done (%d checks so far)\n", g_checks);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// mutations: refused, or a file that verify_cia.py refuses against the original files; never a good-looking wrong file
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
typedef struct {
    int refused, accepted_bad, accepted_good;
} mut_stats;

// `d` is the payload with ONE byte changed. The job is refused (the result is `refused_with`), or it composes a file that is not a good child of the ORIGINAL payload.
static void mutate_payload(int which, size_t pos, mut_stats* st, const char* label, int run_python, int must_refuse) {
    uint8_t* t = (uint8_t*) malloc(which ? PLUG_N : APP_N);
    memcpy(t, which ? PLUG_D : APP_D, which ? PLUG_N : APP_N);
    t[pos] ^= 0x10;
    src_t ta = {which ? APP_D : t, APP_N, 0, 0}, tp = {which ? t : PLUG_D, PLUG_N, 0, 0};
    char out[1024]; snprintf(out, sizeof(out), "%s/mut.cia", O.work);
    dlpc_job j = job_for(0, &ta, &tp, out, NULL);
    faults_off();
    const dlpc_result r = compose(&j);
    CHECK(!exists(P("%s.part", out)), "%s: .part left behind", label);
    CHECK(r != DLPC_OK || !must_refuse, "%s: a byte that the composer must catch (the hash chain of the TMD, the title, the version, the magic) was accepted", label);
    if (r != DLPC_OK) {
        CHECK(r == (which ? DLPC_E_PLUGIN_BAD : DLPC_E_APP_BAD), "%s: refused with %d (%s)", label, (int) r, g_err);
        CHECK(!exists(out), "%s: refused but a file is there", label);
        st->refused++;
    } else {
        size_t n;
        uint8_t* d = load(out, &n);
        char why[200];
        const int good = child_has(d, n, APP_D, APP_N, PLUG_D, PLUG_N, why, sizeof(why)); // is it a good child of the ORIGINAL payload?
        CHECK(!good, "%s: composed a child that looks exactly like the original", label);
        if (run_python && O.verify && O.stage) {
            const vres_t v = run_verify(out, 0, O.stage, 0);
            CHECK(v.ran && !v.valid && v.fail > 0, "%s: verify_cia.py says VALIDO about a child with a flipped byte (%d OK, %d FAIL)", label, v.ok, v.fail);
        }
        free(d);
        st->accepted_bad++;
        remove(out);
    }
    free(t);
}

static void t_mutations(void) {
    printf("== 7. one flipped byte: refused, or a file that verify_cia.py refuses; never a good-looking wrong file\n");
    mut_stats ms = {0, 0, 0}, as = {0, 0, 0}, ps = {0, 0, 0};
    char out[1024]; snprintf(out, sizeof(out), "%s/mut.cia", O.work);
    // --- the model: every byte of the header, the boundaries of every region, and a few hundred more, both variants
    {
        for (int emu = 0; emu < 2; emu++) {
            const uint8_t* m = emu ? M_EMU : M_REAL;
            const size_t mn = emu ? M_EMU_N : M_REAL_N;
            uint8_t* t = (uint8_t*) malloc(mn);
            memcpy(t, m, mn);
            const size_t regions[] = {0x80, 0x80 + 0x18, 0x80 + 0x2040, 0x80 + 0x2A40, 0x80 + 0x2DC0, 0x80 + 0x3900, 0x80 + 0x3A00, 0x80 + 0x3B00, 0x80 + 0x4300, 0x80 + 0x32900 - 1, mn - 0x3AC0,
                                      mn - 0x3AC0 + 0x400, mn - 1};
            for (size_t i = 0; i < 0x80 + 0x40; i++) { // the header of the model, and the first 64 bytes of the CIA
                t[i] ^= 0x20;
                src_t a = {APP_D, APP_N, 0, 0}, p = {PLUG_D, PLUG_N, 0, 0};
                dlpc_job j = job_for(emu, &a, &p, out, NULL);
                j.model = t;
                faults_off();
                const dlpc_result r = compose(&j);
                CHECK(r == DLPC_E_MODEL && !residue(out), "model byte 0x%zX flipped (%s): result %d", i, emu ? "emulator" : "shipping", (int) r);
                ms.refused += r == DLPC_E_MODEL;
                t[i] ^= 0x20;
            }
            for (size_t k = 0; k < sizeof(regions) / sizeof(regions[0]) + (O.quick ? 150 : 600); k++) {
                const size_t i = k < sizeof(regions) / sizeof(regions[0]) ? regions[k] : 0x80 + (rnd() % (mn - 0x80));
                t[i] ^= 0x01;
                src_t a = {APP_D, APP_N, 0, 0}, p = {PLUG_D, PLUG_N, 0, 0};
                dlpc_job j = job_for(emu, &a, &p, out, NULL);
                j.model = t;
                faults_off();
                const dlpc_result r = compose(&j);
                CHECK(r == DLPC_E_MODEL && !residue(out), "model byte 0x%zX flipped (%s): result %d", i, emu ? "emulator" : "shipping", (int) r);
                ms.refused += r == DLPC_E_MODEL;
                t[i] ^= 0x01;
            }
            free(t);
        }
        printf("   model: %d flipped bytes (the header and the first 64 bytes of the CIA, the boundaries of the regions and random places), every one refused\n", ms.refused);
    }
    // --- the app's CIA: the places where a byte can be (header, bitmap, certificates, ticket, TMD signature, TMD body, hash records, content start/middle/end, padding, meta)
    {
        const size_t oc = 0x3900, om = 0x2DC0, hb = om + 0x140;
        // `must`: the byte is covered by something the composer checks (the sizes in the header, the title, the version, the hash chain of the TMD down to the contents), so it
        // MUST be refused. The others (certificates, the ticket's body, the signatures, the content index, the meta block) are not covered by anything: the child that comes out
        // is not the original's and verify_cia.py, given the original files, says so.
        const struct {
            size_t pos;
            const char* what;
            int must;
        } at[] = {{0, "header size", 1}, {0x0C, "ticket size", 1}, {0x18, "content size", 1}, {0x20, "content index", 0}, {0x100, "header padding", 0}, {0x2020, "gap before the certificates", 0},
                  {0x2040, "first certificate", 0}, {0x2A00, "last certificate", 0}, {0x2A40, "ticket signature type", 1}, {0x2A50, "ticket signature", 0},
                  {0x2A40 + 0x140 + 0x9C, "ticket title id", 0}, {0x2DB0, "ticket end", 0}, {om, "TMD signature type", 1}, {om + 8, "TMD signature", 0}, {hb + 0x4C, "TMD title id", 1},
                  {hb + 0x9C, "TMD version", 1}, {hb + 0x9E, "TMD content count", 1}, {hb + 0xA4, "TMD info hash", 1}, {hb + 0xC4 + 4, "TMD info record hash", 1},
                  {hb + 0x9C4, "TMD content id", 0}, {hb + 0x9C4 + 8, "TMD content size", 1}, {hb + 0x9C4 + 0x10, "TMD content hash", 1}, {oc, "first byte of the content", 1},
                  {oc + 0x100, "NCCH magic", 1}, {oc + 0x1000, "content, early", 1}, {oc + 0x4F000, "content, middle", 1}, {oc + 0x9EA00 - 1, "last byte of the content", 1},
                  {oc + 0x9EA00, "first byte of the meta", 0}, {oc + 0x9EA00 + 0x400, "meta SMDH", 0}, {APP_N - 1, "last byte of the file", 0}};
        for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
            const int py = i % 3 == 0 || at[i].pos > oc + 0x9EA00 - 2; // verify_cia.py on a sample (it takes a second or two)
            mutate_payload(0, at[i].pos, &as, P("app byte 0x%zX (%s)", at[i].pos, at[i].what), py && !O.quick, at[i].must);
        }
        for (int k = 0; k < (O.quick ? 20 : 60); k++) {
            const size_t pos = oc + (size_t) (rnd() % 0x9EA00); // inside the content: the hash chain must see it
            mutate_payload(0, pos, &as, P("app, a random byte of the content (0x%zX)", pos), 0, 1);
        }
        for (int k = 0; k < (O.quick ? 5 : 15); k++) {
            mutate_payload(0, rnd() % APP_N, &as, "app, a random byte", 0, 0);
        }
        printf("   app's CIA: %d flipped bytes: %d refused by the composer, %d composed into a child that is NOT the original's (verify_cia.py refuses those it was asked about)\n",
               as.refused + as.accepted_bad, as.refused, as.accepted_bad);
        CHECK(as.refused > 0, "no flipped byte of the app was ever refused");
    }
    // --- the plugin: the header (magic, version) is checked; the body is not covered by anything the composer knows
    {
        const size_t at[] = {0, 3, 4, 7, 8, 9, 10, 11, 12, 15, 16, 100, 4096, PLUG_N / 2, PLUG_N - 1};
        for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
            mutate_payload(1, at[i], &ps, P("plugin byte 0x%zX", at[i]), i % 2 == 0 && !O.quick, at[i] <= 3 || (at[i] >= 9 && at[i] <= 11)); // the magic and the version
        }
        for (int k = 0; k < (O.quick ? 15 : 40); k++) {
            mutate_payload(1, 16 + rnd() % (PLUG_N - 16), &ps, "plugin, a random byte of the body", 0, 0);
        }
        printf("   plugin: %d flipped bytes: %d refused (the magic and the version), %d composed into a child that is NOT the original's\n", ps.refused + ps.accepted_bad, ps.refused, ps.accepted_bad);
        CHECK(ps.refused > 0, "no flipped byte of the plugin was ever refused");
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// the card lies
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
static void t_card(void) {
    printf("== 8. the card lies: short writes, a full card, flipped bytes on the way to the card and back, a failed rename\n");
    char out[1024]; snprintf(out, sizeof(out), "%s/card.cia", O.work);
    const size_t n = COMP_N[0];
    int flips_w = 0, flips_r = 0;
    for (int emu = 0; emu < 2; emu++) {
        const size_t fn = COMP_N[emu];
        // places: the edges of every region of the file, then random places
        size_t pos[400], np = 0;
        const size_t H = 0x32900, R = fn - 0x3AC0 - H;
        const size_t edges[] = {0,      1,         0x18,       0x1F,       0x20,       0x2000,     0x2040,     0x2A3F,     0x2A40,     0x2DC0,     0x2DC0 + 0x140 + 0x9C, 0x3900,
                                0x3900 + 0x104, 0x3900 + 0x18F, 0x3900 + 0x1E0, 0x3B00, 0x3B00 + 0x600, 0x3900 + 0xA00, H - 1,     H,          H + 0x7F,   H + 0x80,   H + 0x1FF,   H + 0x200,
                                H + 0xFFF,      H + 0x1000, H + 0x1100, H + 0x1000 + 0xA5DC0, H + 0x1000 + 0x19CE60, H + R - 0x4000, H + R - 0x3000, H + R - 1, H + R, fn - 0x3AC0 + 0x400, fn - 1};
        for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
            pos[np++] = edges[i];
        }
        const size_t extra = emu ? 20 : (O.quick ? 40 : 90);
        for (size_t i = 0; i < extra; i++) {
            pos[np++] = rnd() % fn;
        }
        for (size_t i = 0; i < np; i++) {
            for (int dir = 0; dir < 2; dir++) {
                if (emu && dir == 1 && (i % 2)) {
                    continue;
                }
                src_t a = {APP_D, APP_N, 0, 0}, p = {PLUG_D, PLUG_N, 0, 0};
                dlpc_job j = job_for(emu, &a, &p, out, NULL);
                faults_off();
                if (dir == 0) {
                    F.fw_mode = FM_CORRUPT;
                    F.fw_at = pos[i];
                } else {
                    F.fr_mode = FM_CORRUPT;
                    F.fr_at = pos[i];
                }
                const dlpc_result r = compose(&j);
                faults_off();
                CHECK(r == DLPC_E_VERIFY, "%s: byte 0x%zX flipped %s the card: result %d (%s)", emu ? "emulator" : "shipping", pos[i], dir ? "coming back from" : "on the way to", (int) r, g_err);
                CHECK(!residue(out), "%s: byte 0x%zX flipped: a file is left behind", emu ? "emulator" : "shipping", pos[i]);
                if (dir) {
                    flips_r++;
                } else {
                    flips_w++;
                }
            }
        }
    }
    printf("   a flipped byte on the way to the card: %d places, on the way back: %d places; every one is refused by the read-back check (E_VERIFY) and nothing is left\n", flips_w, flips_r);
    // a short write: at the first, the middle, the last chunk, and just before the end; a full card says so
    const uint64_t cuts[] = {0, 1, 65535, 65536, 65537, n / 2, n - 65536, n - 1};
    for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
        for (int full = 0; full < 2; full++) {
            src_t a = {APP_D, APP_N, 0, 0}, p = {PLUG_D, PLUG_N, 0, 0};
            dlpc_job j = job_for(0, &a, &p, out, NULL);
            faults_off();
            F.fw_mode = FM_SHORT;
            F.fw_at = cuts[i];
            F.fw_errno = full ? ENOSPC : EIO;
            const dlpc_result r = compose(&j);
            faults_off();
            CHECK(r == DLPC_E_WRITE && !residue(out), "short write at %llu: result %d (%s)", (unsigned long long) cuts[i], (int) r, g_err);
            CHECK(full ? strstr(g_err, "full") != NULL : strstr(g_err, "refused") != NULL, "short write sentence: '%s'", g_err);
        }
    }
    // a read-back that ends early or is not there
    for (size_t i = 0; i < 4; i++) {
        const uint64_t at = i == 0 ? 0 : i == 1 ? 70000 : i == 2 ? n / 2 : n - 1;
        src_t a = {APP_D, APP_N, 0, 0}, p = {PLUG_D, PLUG_N, 0, 0};
        dlpc_job j = job_for(0, &a, &p, out, NULL);
        faults_off();
        F.fr_mode = FM_SHORT;
        F.fr_at = at;
        const dlpc_result r = compose(&j);
        faults_off();
        CHECK(r == DLPC_E_VERIFY && !residue(out), "read-back ends at %llu: result %d (%s)", (unsigned long long) at, (int) r, g_err);
    }
    {
        src_t a = {APP_D, APP_N, 0, 0}, p = {PLUG_D, PLUG_N, 0, 0};
        dlpc_job j = job_for(0, &a, &p, out, NULL);
        faults_off();
        F.rename_fail = 1;
        const dlpc_result r = compose(&j);
        faults_off();
        CHECK(r == DLPC_E_WRITE && !residue(out), "rename fails: result %d (%s)", (int) r, g_err);
    }
    printf("   short writes (empty, one chunk, middle, last byte), a full card, a read-back that ends early, a failed rename: refused, nothing left behind\n");
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// synthetic payloads, the limit, the model rules
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// An app CIA of the real app's header, certificates, ticket and TMD with a content of `content` random bytes and the TMD's hashes made again. with_meta = the
// real app's meta block after the content (padded to 64); else the file ends with the content (the last 63 bytes of padding are not there: the installer accepts it).
static uint8_t* synth_app(size_t content, int with_meta, uint32_t seed, size_t* out_n) {
    const size_t oc = 0x3900, hb = 0x2DC0 + 0x140, cr = hb + 0x9C4, info = hb + 0xC4, meta = with_meta ? 0x3AC0 : 0;
    const size_t body = with_meta ? (size_t) al(content, 64) : content;
    uint8_t* d = (uint8_t*) calloc(1, oc + body + meta + 64);
    memcpy(d, APP_D, oc);
    wr32(d + 0x14, (uint32_t) meta);
    wr64(d + 0x18, al(content, 64));
    fill_random(d + oc, content, seed);
    uint8_t h[32];
    sha256(d + oc, content, h);
    wb64(d + cr + 8, content);
    memcpy(d + cr + 0x10, h, 32);
    for (int k = 0; k < 64; k++) {
        if (rb32(d + info + 0x24 * (size_t) k) & 0xFFFF) {
            sha256(d + cr, 0x30, d + info + 0x24 * (size_t) k + 4);
        }
    }
    sha256(d + info, 0x24 * 64, d + hb + 0xA4);
    if (with_meta) {
        memcpy(d + oc + body, APP_D + APP_N - 0x3AC0, 0x3AC0);
    }
    *out_n = oc + body + meta;
    return d;
}
static uint8_t* synth_plugin(size_t n, uint32_t seed) {
    uint8_t* d = (uint8_t*) malloc(n);
    fill_random(d, n, seed);
    memcpy(d, PLUG_D, n < 16 ? n : 16);
    return d;
}

static void t_sizes(void) {
    printf("== 9. synthetic payloads around the block boundaries, the 32 MiB limit to the byte, the model rules\n");
    const struct {
        size_t content, plugin;
        int meta, emu, python;
    } cases[] = {{1, 16, 1, 0, 1},          {0x3FFF, 17, 0, 0, 0},      {4096, 4095, 1, 0, 1},     {65536, 65537, 0, 1, 1},   {300000, 4096, 1, 0, 1},    {1 << 20, 1500000, 0, 1, 0},
                 {0x80000 - 12, 100, 1, 0, 0}, {0x80000 + 3, 4097, 0, 0, 0}, {5 << 20, 7 << 20, 1, 0, 1}, {61, 63, 0, 1, 0},  {0x3C0, 0x2000, 1, 1, 0}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t an;
        uint8_t* app = synth_app(cases[i].content, cases[i].meta, (uint32_t) i + 1, &an);
        uint8_t* plug = synth_plugin(cases[i].plugin, (uint32_t) i + 100);
        src_t a = {app, an, 0, 0}, p = {plug, cases[i].plugin, 0, 0};
        char out[1024]; snprintf(out, sizeof(out), "%s/syn%zu.cia", O.work, i);
        prog_t pr;
        memset(&pr, 0, sizeof(pr));
        dlpc_job j = job_for(cases[i].emu, &a, &p, out, &pr);
        faults_off();
        const dlpc_result r = compose(&j);
        CHECK(r == DLPC_OK, "synthetic case %zu (content %zu, plugin %zu): %s", i, cases[i].content, cases[i].plugin, g_err);
        if (r == DLPC_OK) {
            size_t n;
            uint8_t* d = load(out, &n);
            char why[200];
            CHECK(n == dlpc_output_size(&j), "case %zu: size %zu, announced %llu", i, n, (unsigned long long) dlpc_output_size(&j));
            CHECK(child_has(d, n, app, an, plug, cases[i].plugin, why, sizeof(why)), "case %zu: %s", i, why);
            prog_expect_ok(&pr, "synthetic");
            if (cases[i].python && O.verify && !O.quick) {
                char dir[1024]; snprintf(dir, sizeof(dir), "%s/synstage", O.work);
                run_cmd(P("rm -rf '%s' && mkdir -p '%s'", dir, dir));
                save(P("%s/3dslink.cia", dir), app, an);
                save(P("%s/AzaharLink.3gx", dir), plug, cases[i].plugin);
                char man[512];
                const size_t mn = expect_manifest(man, sizeof(man), app, an, plug, cases[i].plugin);
                save(P("%s/manifest.txt", dir), man, mn);
                const vres_t v = run_verify(out, cases[i].emu, dir, 1);
                CHECK(v.ran && v.valid && v.fail == 0, "case %zu: verify_cia.py: %d OK %d FAIL", i, v.ok, v.fail);
                printf("   case %zu (app %zu bytes %s, plugin %zu, %s): verify_cia.py %d OK, %d FAIL\n", i, an, cases[i].meta ? "with meta" : "without meta", cases[i].plugin,
                       cases[i].emu ? "emulator" : "shipping", v.ok, v.fail);
            }
            free(d);
        }
        remove(out);
        free(app);
        free(plug);
    }
    printf("   %zu synthetic payloads composed, extracted and checked against their inputs\n", sizeof(cases) / sizeof(cases[0]));

    // --- the limit: the largest plugin that keeps the child at or under 32 MiB composes (and Download Play's host library accepts it), one more byte does not
    {
        size_t an;
        uint8_t* app = synth_app(3u << 20, 1, 77, &an);
        src_t a = {app, an, 0, 0}, p0 = {PLUG_D, PLUG_N, 0, 0};
        char limit_path[1024];
        snprintf(limit_path, sizeof(limit_path), "%s/limit.cia", O.work);
        dlpc_job j = job_for(0, &a, &p0, limit_path, NULL);
        size_t lo = 16, hi = 0x02000000;
        while (lo < hi) { // the largest plugin size whose child fits
            const size_t mid = lo + (hi - lo + 1) / 2;
            j.plugin_size = mid;
            if (dlpc_output_size(&j) <= 0x02000000ull) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }
        j.plugin_size = lo;
        // The RomFS grows by one block of 4096 bytes at a time, and by two when the level of hashes above its data grows with it (a block of the second level holds the hashes of 128
        // blocks of data: 512 KB): so the largest child is under the limit by less than TWO blocks. (With the model of 1.0.0 as the first build made it, the first plugin size that did
        // not fit was one of those steps of 8192: the check said "less than 4096" and failed on the byte that was only the size of the model.)
        const uint64_t biggest = dlpc_output_size(&j);
        CHECK(biggest <= 0x02000000ull && 0x02000000ull - biggest < 2 * 4096, "the largest child that fits is %llu bytes (the limit is 33554432)", (unsigned long long) biggest);
        j.plugin_size = lo + 1;
        CHECK(dlpc_output_size(&j) > 0x02000000ull, "one byte more must not fit");
        uint8_t* plug = synth_plugin(lo + 1, 5);
        src_t p = {plug, lo + 1, 0, 0};
        j.plugin_user = &p;
        j.plugin_size = lo + 1;
        try_job(j, "one byte past the limit", DLPC_E_TOOBIG);
        p.n = lo;
        j.plugin_size = lo;
        faults_off();
        const double t0 = now_s();
        const dlpc_result r = compose(&j);
        CHECK(r == DLPC_OK, "the largest child that fits (plugin %zu): %s", lo, g_err);
        if (r == DLPC_OK) {
            size_t n;
            uint8_t* d = load(j.out_path, &n);
            char why[200];
            CHECK(n == biggest && child_has(d, n, app, an, plug, lo, why, sizeof(why)), "the largest child: %zu bytes, %s", n, why);
            char nm[80];
            unsigned ver;
            unsigned long long tid;
            unsigned long cb, tb, bl;
            CHECK(dlpc_glue_dlph(j.out_path, nm, sizeof(nm), &ver, &tid, &cb, &tb, &bl) && cb == n, "dlph_open on the largest child");
            free(d);
            printf("   the largest child (app %zu + plugin %zu): %zu bytes (the limit is 33554432), composed in %.1f s, accepted by dlph_open; one byte more: refused (E_TOOBIG)\n", an, lo, n,
                   now_s() - t0);
        }
        remove(j.out_path);
        free(app);
        free(plug);
    }

    // --- the model rules: C (dlpc.c) and make_model.py (--check) must agree on a model whose checksum is right but whose structure is not
    if (O.agree && O.tools) {
        char dir[1024]; snprintf(dir, sizeof(dir), "%s/agree", O.work);
        run_cmd(P("rm -rf '%s' && mkdir -p '%s'", dir, dir));
        const size_t oc = 0x3900, om = 0x2DC0, hb = om + 0x140, tb = 0x2A40 + 0x140, cr = hb + 0x9C4, mh = 0x80;
        size_t list[1200], nl = 0;
        for (size_t i = 4; i < 0x60; i++) list[nl++] = i;                       // the fields of the model's header
        for (size_t i = 0; i < 0x20; i++) list[nl++] = mh + i;                  // the header of the CIA
        for (size_t i = 0; i < 12; i++) list[nl++] = mh + tb + 0x9C + i;        // ticket: title id, version
        for (size_t i = 0; i < 8; i++) list[nl++] = mh + hb + 0x4C + i;         // TMD: title id
        for (size_t i = 0; i < 4; i++) list[nl++] = mh + hb + 0x9C + i;         // TMD: version, count
        for (size_t i = 0; i < 0x30; i++) list[nl++] = mh + cr + i;             // TMD: the content record
        for (size_t i = 0; i < 0x200; i++) list[nl++] = mh + oc + i;            // the NCCH header
        for (size_t i = 0; i < 0x20; i++) list[nl++] = mh + oc + 0x200 + i;     // the start of the exheader (the rest is covered by the hash regions, not by rules)
        for (size_t i = 0; i < 0x60; i++) list[nl++] = mh + oc + 0xA00 + i;     // the start of the ExeFS
        for (size_t i = 0; i < 0x20; i++) list[nl++] = mh + 0x32900 + 0x400 + i;  // the meta block: the SMDH magic and what follows
        int made = 0;
        for (int emu = 0; emu < 2; emu++) {
            const uint8_t* m = emu ? M_EMU : M_REAL;
            const size_t mn = emu ? M_EMU_N : M_REAL_N;
            uint8_t* t = (uint8_t*) malloc(mn);
            for (size_t k = 0; k < nl; k++) {
                if (list[k] >= mn) {
                    continue;
                }
                for (int v = 0; v < 2; v++) {
                    memcpy(t, m, mn);
                    t[list[k]] = v ? 0xFF : (uint8_t) (t[list[k]] ^ 1);
                    sha256_ctx sc; // (the checksum is made again, also when the flip is inside the header)
                    sha256_init(&sc);
                    sha256_update(&sc, t, 0x60);
                    sha256_update(&sc, t + 0x80, mn - 0x80);
                    sha256_final(&sc, t + 0x60);
                    save(P("%s/%d_%05zu_%d.bin", dir, emu, list[k], v), t, mn);
                    made++;
                }
            }
            free(t);
        }
        const int rc = run_cmd(P("%s -I '%s' '%s' '%s'", O.python, O.agree, O.tools, dir));
        CHECK(rc == 0, "dlpc_model_agree.py failed (%d): %.200s", rc, g_cmd_out);
        int agree = 0, disagree = 0, rejected = 0, accepted = 0;
        for (char* line = strtok(g_cmd_out, "\n"); line; line = strtok(NULL, "\n")) {
            char name[200], verdict[16];
            if (sscanf(line, "%199s %15s", name, verdict) != 2) {
                continue;
            }
            size_t mn;
            uint8_t* t = load(P("%s/%s", dir, name), &mn);
            const int c_ok = dlpc_model_version(t, mn) != 0, py_ok = strcmp(verdict, "ok") == 0;
            free(t);
            if (c_ok == py_ok) {
                agree++;
            } else {
                disagree++;
                CHECK(0, "the model rules differ on %s: C says %s, make_model.py says %s", name, c_ok ? "ok" : "bad", verdict);
            }
            rejected += !c_ok;
            accepted += c_ok;
        }
        CHECK(agree + disagree == made, "make_model.py judged %d of %d models", agree + disagree, made);
        CHECK(rejected >= 300 && accepted > 0, "the mutations are not a good test: %d rejected, %d accepted", rejected, accepted);
        printf("   model rules: %d models with a right checksum and a changed byte: C and make_model.py agree on all (%d rejected, %d accepted)\n", agree, rejected, accepted);
        run_cmd(P("rm -rf '%s'", dir));
    } else {
        printf("   (no --agree/--tools: the model rules against make_model.py are skipped)\n");
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// the speed: what each stage costs on the PC (no sanitizers: build without them)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
static void t_bench(void) {
    printf("== bench (PC; the console is far slower: assume 0.5-1 MB/s for the software AES and 1-3 MB/s for the SHA-256 on an old 3DS)\n");
    const size_t big = 32u << 20;
    uint8_t* buf = (uint8_t*) malloc(big);
    fill_random(buf, big, 9);
    uint8_t h[32];
    double t = now_s();
    sha256(buf, big, h);
    const double sha_s = now_s() - t;
    uint32_t rk[44];
    uint8_t zero[16], ctr[16];
    memset(zero, 0, 16);
    memset(ctr, 3, 16);
    aes_init();
    aes_set_key(rk, zero);
    t = now_s();
    aes_ctr_crypt(rk, ctr, 0, buf, big);
    const double aes_s = now_s() - t;
    t = now_s();
    uint8_t* cp = (uint8_t*) malloc(big);
    memcpy(cp, buf, big);
    memset(cp, 0, big);
    const double mem_s = now_s() - t;
    free(cp);
    free(buf);
    const double sha_bps = (double) big / sha_s, aes_bps = (double) big / aes_s;
    printf("   SHA-256: %.1f MB/s   AES-128-CTR: %.1f MB/s   (titleshare/common sha256.c and aes.c, this machine)\n", sha_bps / 1e6, aes_bps / 1e6);
    (void) mem_s;
    double best[8][2] = {{0}};
    uint64_t units[8] = {0};
    char out[1024]; snprintf(out, sizeof(out), "%s/bench.cia", O.work);
    double total_best = 1e9;
    for (int rep = 0; rep < 5; rep++) {
        src_t a = {APP_D, APP_N, 0, 0}, p = {PLUG_D, PLUG_N, 0, 0};
        prog_t pr;
        memset(&pr, 0, sizeof(pr));
        dlpc_job j = job_for(0, &a, &p, out, &pr);
        const double t0 = now_s();
        const dlpc_result r = compose(&j);
        const double dt = now_s() - t0;
        if (r != DLPC_OK) {
            printf("   bench: compose failed: %s\n", g_err);
            return;
        }
        for (int s = 1; s <= 6; s++) {
            const double d = pr.t1[s] - pr.t0[s];
            if (rep == 0 || d < best[s][0]) {
                best[s][0] = d;
            }
            units[s] = pr.total[s];
        }
        if (dt < total_best) {
            total_best = dt;
        }
        remove(out);
    }
    src_t a = {APP_D, APP_N, 0, 0}, p = {PLUG_D, PLUG_N, 0, 0};
    dlpc_job j = job_for(0, &a, &p, out, NULL);
    printf("   stage                                 progress units   measured     units/s   predicted from the work by kind (SHA %.0f MB/s, AES %.0f MB/s, files in RAM)\n", sha_bps / 1e6,
           aes_bps / 1e6);
    double pred_total = 0;
    for (int s = 1; s <= 6; s++) {
        const dlpc_work w = dlpc_stage_work(&j, s);
        const double pred = (double) w.sha / sha_bps + (double) w.aes / aes_bps;
        pred_total += pred;
        printf("   %d %-34s %12llu   %7.1f ms   %7.1f MB/s   %7.1f ms   [read %llu write %llu sha %llu aes %llu bytes]\n", s, dlpc_stage_text(s), (unsigned long long) units[s], best[s][0] * 1e3,
               best[s][0] > 0 ? (double) units[s] / best[s][0] / 1e6 : 0.0, pred * 1e3, (unsigned long long) w.sd_read, (unsigned long long) w.sd_write, (unsigned long long) w.sha,
               (unsigned long long) w.aes);
    }
    printf("   whole job (best of 5): %.1f ms; predicted by SHA and AES alone: %.1f ms\n", total_best * 1e3, pred_total * 1e3);
    const dlpc_work all[6] = {dlpc_stage_work(&j, 1), dlpc_stage_work(&j, 2), dlpc_stage_work(&j, 3), dlpc_stage_work(&j, 4), dlpc_stage_work(&j, 5), dlpc_stage_work(&j, 6)};
    uint64_t sha = 0, aes = 0, rd = 0, wr = 0;
    for (int s = 0; s < 6; s++) {
        sha += all[s].sha;
        aes += all[s].aes;
        rd += all[s].sd_read;
        wr += all[s].sd_write;
    }
    printf("   the whole job by kind of work: SHA-256 %llu bytes, AES %llu bytes, card read %llu bytes, card write %llu bytes\n", (unsigned long long) sha, (unsigned long long) aes,
           (unsigned long long) rd, (unsigned long long) wr);
    for (int lo = 0; lo < 3; lo++) { // what it would take on a console: SHA 1 / 2 / 3 MB/s, AES 0.5 / 0.75 / 1 MB/s, card 6 MB/s read 3 MB/s write
        const double shas = lo == 0 ? 1e6 : lo == 1 ? 2e6 : 3e6, aess = lo == 0 ? 0.5e6 : lo == 1 ? 0.75e6 : 1e6;
        printf("   on a console with SHA %.0f MB/s and AES %.2f MB/s (card 6 MB/s read, 3 MB/s write): about %.0f s\n", shas / 1e6, aess / 1e6,
               (double) sha / shas + (double) aes / aess + (double) rd / 6e6 + (double) wr / 3e6);
    }
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--work") && v) O.work = argv[++i];
        else if (!strcmp(a, "--app") && v) O.app = argv[++i];
        else if (!strcmp(a, "--plugin") && v) O.plugin = argv[++i];
        else if (!strcmp(a, "--model") && v) O.model = argv[++i];
        else if (!strcmp(a, "--model-emu") && v) O.model_emu = argv[++i];
        else if (!strcmp(a, "--ref") && v) O.ref = argv[++i];
        else if (!strcmp(a, "--ref-emu") && v) O.ref_emu = argv[++i];
        else if (!strcmp(a, "--stage") && v) O.stage = argv[++i];
        else if (!strcmp(a, "--verify") && v) O.verify = argv[++i];
        else if (!strcmp(a, "--python") && v) O.python = argv[++i];
        else if (!strcmp(a, "--ctrtool") && v) O.ctrtool = argv[++i];
        else if (!strcmp(a, "--tools") && v) O.tools = argv[++i];
        else if (!strcmp(a, "--agree") && v) O.agree = argv[++i];
        else if (!strcmp(a, "--elf") && v) O.elf = argv[++i];
        else if (!strcmp(a, "--tid") && v) O.tid = strtoull(argv[++i], NULL, 16);
        else if (!strcmp(a, "--version16") && v) O.v16 = (unsigned) strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--version-text") && v) O.vtext = argv[++i];
        else if (!strcmp(a, "--expect") && i + 3 < argc) {
            O.name = argv[i + 1];
            O.desc = argv[i + 2];
            O.author = argv[i + 3];
            i += 3;
        } else if (!strcmp(a, "--bench")) O.bench = 1;
        else if (!strcmp(a, "--quick")) O.quick = 1;
        else usage();
    }
    if (!O.work || !O.app || !O.plugin || !O.model || !O.model_emu) {
        usage();
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    mkdir(O.work, 0777);
    APP_D = load(O.app, &APP_N);
    PLUG_D = load(O.plugin, &PLUG_N);
    M_REAL = load(O.model, &M_REAL_N);
    M_EMU = load(O.model_emu, &M_EMU_N);
    printf("dlpc_test: app %zu bytes, plugin %zu bytes, models %zu/%zu bytes, version %s = %u, title %016llX, sizeof(size_t) = %zu\n", APP_N, PLUG_N, M_REAL_N, M_EMU_N, O.vtext, O.v16,
           (unsigned long long) O.tid, sizeof(size_t));
    if (O.bench) {
        t_bench();
        return g_failed != 0;
    }
    t_reference();
    t_relation();
    t_external();
    t_acceptors();
    t_virtual();
    t_cancel();
    t_errors();
    t_mutations();
    t_card();
    t_sizes();
    printf("\n%d checks, %d failed -- %s\n", g_checks, g_failed, g_failed ? "FAILED" : "ALL OK");
    return g_failed != 0;
}
