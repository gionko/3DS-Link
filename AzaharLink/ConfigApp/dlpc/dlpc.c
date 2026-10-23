// dlpc.c -- the composer of the Download Play child of 3DS Link. Contract: dlpc.h. Design: DlpLink/docs/COMPOSITORE.md. Proof: DlpLink/tests/dlpc_test.c.
//
// It makes the file that DlpLink/tools/make_dlp_child.sh (makerom) and finalize_child.py make, from the MODEL (the finished child without its RomFS, DlpLink/tools/
// make_model.py) and the payload (the app's CIA, the plugin): byte for byte the same file when the payload is the same.
//
// The child is built whole in ONE block of memory, the finished file (the head of the model, the RomFS, the meta: about 2 MB for 3DS Link 1.0). The six stages:
//   READ_APP     the app's CIA is read straight into its place in the RomFS and hashed; its TMD chain is checked (a flipped byte in it is refused here)
//   READ_PLUGIN  the same for the plugin (a .3gx of this version)
//   TREE         manifest.txt, the RomFS directory tables, the IVFC hash levels and the master hash
//   SEAL         NCCH header (sizes, hash of the RomFS header), zero-key AES-128-CTR of the RomFS, TMD chain over the stored (encrypted) bytes, CIA header
//   WRITE        "<out>.part" goes to the card
//   VERIFY       the file is read back into the same block and checked as if it came from somebody else: CIA/TMD, the head against the model, NCCH, the
//                conditions of dlph_open and of the installer, the ExeFS/exheader hashes, the RomFS decrypted and its IVFC tree level by level, the
//                directory tables, the manifest and the hash of every file in it. Only then is the file renamed.
// Every size is bounded (32 MiB for each input, 32 MiB for the child) BEFORE any arithmetic and the arithmetic is in 64 bits until a result is known to be
// small, so size_t (32 bits on the console) and the 32-bit fields of the formats cannot overflow.
#include "dlpc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../titleshare/common/aes.h"
#include "../titleshare/common/sha256.h"

#define MAX_CHILD 0x02000000u // what dlph_open accepts (the `required` size of the beacon): 32 MiB
#define MODEL_HDR 0x80u       // the header of a model (make_model.py)
#define MODEL_FORMAT 1u
#define MODEL_MAX_BODY 0x400000u
#define META_SIZE 0x3AC0u
#define CHUNK 65536u // the callbacks, the reads, the writes and the progress go in pieces of this size
#define BLK 4096u    // IVFC block size (log2 12)
#define NONE32 0xFFFFFFFFu

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// little helpers
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
static uint16_t le16(const uint8_t* p) {
    return (uint16_t) (p[0] | (p[1] << 8));
}
static uint32_t le32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}
static uint64_t le64(const uint8_t* p) {
    return (uint64_t) le32(p) | ((uint64_t) le32(p + 4) << 32);
}
static uint16_t be16(const uint8_t* p) {
    return (uint16_t) ((p[0] << 8) | p[1]);
}
static uint32_t be32(const uint8_t* p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}
static uint64_t be64(const uint8_t* p) {
    return ((uint64_t) be32(p) << 32) | be32(p + 4);
}
static void put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16);
    p[3] = (uint8_t) (v >> 24);
}
static void put_le64(uint8_t* p, uint64_t v) {
    put_le32(p, (uint32_t) v);
    put_le32(p + 4, (uint32_t) (v >> 32));
}
static void put_be64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t) (v >> (56 - 8 * i));
    }
}
// Round up to a power of two; every caller passes values far below 2^40.
static uint64_t al(uint64_t x, uint64_t a) {
    return (x + a - 1) & ~(a - 1);
}

// A string builder over a fixed buffer: past the end it only counts and says `over` (the manifest is a few hundred bytes).
typedef struct {
    char* p;
    size_t cap, len;
    int over;
} sb_t;
static void sb_ch(sb_t* s, char ch) {
    if (s->len + 1 < s->cap) {
        s->p[s->len] = ch;
    } else {
        s->over = 1;
    }
    s->len++;
}
static void sb_str(sb_t* s, const char* t) {
    while (*t) {
        sb_ch(s, *t++);
    }
}
static void sb_dec(sb_t* s, uint64_t v) {
    char t[24];
    int n = 0;
    do {
        t[n++] = (char) ('0' + (int) (v % 10));
        v /= 10;
    } while (v);
    while (n) {
        sb_ch(s, t[--n]);
    }
}
static void sb_hex(sb_t* s, uint64_t v, int digits, int upper) {
    const char* a = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) {
        sb_ch(s, a[(v >> (4 * i)) & 15]);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// CIA layout (the same rules as make_model.py cia_layout and verify_cia.py)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
typedef struct {
    uint32_t certsz, tiksz, tmdsz, metasz, n; // n = number of contents in the TMD
    uint64_t csz;                             // content size of the CIA header
    uint64_t ot, om, oc;                      // ticket, TMD, content 0 (offsets in the file)
    uint64_t hb, tb;                          // TMD header body, ticket body
    uint64_t cr, info;                        // content chunk records, content info records
} cia_lay;

// The bytes that follow the type of a signature up to the body that it signs: type (4) + signature + padding; 0 for an unknown type.
static uint32_t sig_skip(uint32_t type) {
    switch (type) {
    case 0x10000:
    case 0x10003:
        return 4 + 0x200 + 0x3C;
    case 0x10001:
    case 0x10004:
        return 4 + 0x100 + 0x3C;
    case 0x10002:
    case 0x10005:
        return 4 + 0x3C + 0x40;
    default:
        return 0;
    }
}

// NULL when `d` (len bytes) starts like a CIA that this library can read, else the reason. Nothing is read past len.
static const char* cia_layout(const uint8_t* d, uint64_t len, cia_lay* L) {
    memset(L, 0, sizeof(*L));
    if (len < 0x2040) {
        return "it is too short to be a CIA";
    }
    if (le32(d) != 0x2020 || le16(d + 4) != 0 || le16(d + 6) != 0) {
        return "its header is not that of a CIA";
    }
    L->certsz = le32(d + 8);
    L->tiksz = le32(d + 12);
    L->tmdsz = le32(d + 16);
    L->metasz = le32(d + 20);
    L->csz = le64(d + 0x18);
    if (L->certsz == 0 || L->certsz > 0x10000 || L->tiksz == 0 || L->tiksz > 0x10000 || L->tmdsz == 0 || L->tmdsz > 0x10000) {
        return "the sizes of its certificates, ticket or TMD are out of range";
    }
    L->ot = 0x2040 + al(L->certsz, 64);
    L->om = L->ot + al(L->tiksz, 64);
    L->oc = L->om + al(L->tmdsz, 64);
    if (L->oc > len) {
        return "it ends before its content";
    }
    uint32_t skip = sig_skip(be32(d + L->om));
    if (skip == 0) {
        return "the signature type of its TMD is unknown";
    }
    L->hb = L->om + skip;
    skip = sig_skip(be32(d + L->ot));
    if (skip == 0) {
        return "the signature type of its ticket is unknown";
    }
    L->tb = L->ot + skip;
    if (L->hb + 0x9C4 + 0x30 > L->om + L->tmdsz || L->tb + 0xB0 > L->ot + L->tiksz) {
        return "its TMD or ticket is too short";
    }
    L->n = be16(d + L->hb + 0x9E);
    L->info = L->hb + 0xC4;
    L->cr = L->hb + 0x9C4;
    if (L->n == 0 || L->cr + 0x30ull * L->n > L->om + L->tmdsz) {
        return "the content records do not fit in its TMD";
    }
    return NULL;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// The model (make_model.py is the other half of this contract: parse_model there and model_parse here apply the same rules)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
typedef struct {
    uint16_t version;
    uint32_t body, head, meta, tmd_off, tmd_size, ncch_off, romfs_off;
    uint64_t tid;
    int emu;
    const uint8_t* head_p; // the first `head` bytes of the child CIA: up to the place of the RomFS in the NCCH
    const uint8_t* meta_p; // the meta block
    uint32_t hr;           // bytes of the RomFS header that the NCCH hashes (units of 0x200 at NCCH+0x1B8)
    cia_lay L;             // layout of head_p
} model_info;

// NULL when `m` is a model, else the reason. check_sum = 0 skips the SHA-256 (for the size questions: the real job does the full check).
static const char* model_parse(const uint8_t* m, size_t n, int check_sum, model_info* mi) {
    memset(mi, 0, sizeof(*mi));
    if (m == NULL || n < MODEL_HDR || memcmp(m, "DLPM", 4) != 0) {
        return "it is not a model";
    }
    if (le32(m + 4) != MODEL_FORMAT || le32(m + 8) != MODEL_HDR) {
        return "unknown format";
    }
    const uint32_t flags = le32(m + 12);
    if (flags & ~1u) {
        return "unknown flags";
    }
    mi->version = le16(m + 0x10);
    mi->body = le32(m + 0x14);
    mi->head = le32(m + 0x18);
    mi->meta = le32(m + 0x1C);
    mi->tmd_off = le32(m + 0x20);
    mi->tmd_size = le32(m + 0x24);
    mi->ncch_off = le32(m + 0x28);
    mi->romfs_off = le32(m + 0x2C);
    mi->tid = le64(m + 0x30);
    mi->emu = (int) (flags & 1);
    if (le16(m + 0x12) != 0) {
        return "reserved fields are not zero";
    }
    for (int i = 0x38; i < 0x60; i++) {
        if (m[i] != 0) {
            return "reserved fields are not zero";
        }
    }
    if (mi->body > MODEL_MAX_BODY || (uint64_t) n != MODEL_HDR + (uint64_t) mi->body || (uint64_t) mi->head + mi->meta != mi->body) {
        return "inconsistent sizes";
    }
    if (check_sum) {
        sha256_ctx s;
        uint8_t h[32];
        sha256_init(&s);
        sha256_update(&s, m, 0x60);
        sha256_update(&s, m + MODEL_HDR, mi->body);
        sha256_final(&s, h);
        if (memcmp(h, m + 0x60, 32) != 0) {
            return "checksum mismatch";
        }
    }
    const uint8_t* b = m + MODEL_HDR;
    const char* why = cia_layout(b, mi->head, &mi->L);
    if (why != NULL) {
        return why;
    }
    const cia_lay* L = &mi->L;
    if (L->oc != mi->ncch_off || L->om != mi->tmd_off || L->tmdsz != mi->tmd_size || L->metasz != mi->meta || mi->meta != META_SIZE) {
        return "offsets differ from the CIA";
    }
    if (le64(b + 0x18) != 0) {
        return "the content size is not zeroed";
    }
    if (mi->romfs_off < 0xA00 || mi->romfs_off % 0x200 != 0 || (uint64_t) mi->head != (uint64_t) mi->ncch_off + mi->romfs_off || mi->head > mi->body) {
        return "the place of the RomFS is out of range";
    }
    if (L->n != 1) {
        return "the TMD must have one content";
    }
    if (be16(b + L->hb + 0x9C) != mi->version || be16(b + L->tb + 0xA6) != mi->version) {
        return "the version of the TMD or of the ticket differs";
    }
    if (be64(b + L->hb + 0x4C) != mi->tid || be64(b + L->tb + 0x9C) != mi->tid || (mi->tid >> 32) != 0x00040001u) {
        return "the Title ID is not that of a Download Play child";
    }
    if (be64(b + L->cr + 8) != 0 || (be16(b + L->cr + 6) & 1)) {
        return "the content record is not blank or is encrypted at the CIA level";
    }
    const uint8_t* nc = b + L->oc; // the NCCH: ncch_off + 0xA00 <= head, so the header and the exheader are inside
    if (memcmp(nc + 0x100, "NCCH", 4) != 0 || le64(nc + 0x108) != mi->tid || le64(nc + 0x118) != mi->tid) {
        return "no NCCH, or one of another title";
    }
    if (le32(nc + 0x104) != 0 || le32(nc + 0x1B4) != 0) {
        return "the NCCH already has a size and a RomFS (is this the whole child?)";
    }
    const uint32_t rhr = le32(nc + 0x1B8);
    if ((uint64_t) le32(nc + 0x1B0) * 0x200 != mi->romfs_off || rhr < 1 || rhr > 8) {
        return "the NCCH has no place for the RomFS that can be filled";
    }
    mi->hr = rhr * 0x200;
    const uint64_t ex_off = (uint64_t) le32(nc + 0x1A0) * 0x200, ex_size = (uint64_t) le32(nc + 0x1A4) * 0x200;
    const uint32_t ehr = le32(nc + 0x1A8);
    if (le32(nc + 0x180) != 0x400 || ex_off < 0xA00 || ex_off + ex_size > mi->romfs_off || ehr < 1 || ehr > 8 || (uint64_t) ehr * 0x200 > ex_size) {
        return "the exheader or the ExeFS is out of place";
    }
    const uint8_t* fl = nc + 0x188;
    if ((mi->emu && !(fl[7] & 4)) || (!mi->emu && fl[7] != 0x01) || fl[5] != 0x03) {
        return "the NCCH flags are not those of its variant";
    }
    mi->head_p = b;
    mi->meta_p = b + mi->head;
    if (memcmp(mi->meta_p + 0x400, "SMDH", 4) != 0) {
        return "the meta block holds no SMDH";
    }
    return NULL;
}

uint16_t dlpc_model_version(const uint8_t* model, size_t model_size) {
    model_info mi;
    return model_parse(model, model_size, 1, &mi) == NULL ? mi.version : 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// The layout of the child that a job makes
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
enum { F_APP = 0, F_PLUGIN = 1, F_MANIFEST = 2, NFILES = 3 };
// makerom sorts the files of a directory by name (code points); these three are in that order. The buckets of the name hash tables are 3 for 3 files.
static const char* const kNames[NFILES] = {"3dslink.cia", "AzaharLink.3gx", "manifest.txt"};
#define NBUCKETS 3u

typedef struct {
    uint32_t head, meta, romfs_off;                       // the file: head | RomFS region | meta
    uint32_t region, ncch_size, file_size;                // RomFS region bytes; NCCH bytes (= the TMD content size); file bytes
    uint32_t fsize[NFILES], fdata[NFILES];                // the files of the RomFS: size and offset in the data area
    uint32_t fent[NFILES], fnlen[NFILES];                 // offset of its entry in the file meta table; its name in characters
    uint32_t dh_o, dh_s, dm_o, dm_s, fh_o, fh_s, fm_o, fm_s, fd_o; // the level 3 header
    uint32_t s1, s2, s3, mh;                              // IVFC: bytes of level 1, 2, 3 and of the master hash
    uint32_t nb1, nb2, nb3;                               // blocks of level 1, 2, 3
    uint32_t p3, p1, p2;                                  // physical places in the region: level 3, level 1, level 2 (0x60 + master hash comes first)
} layout;

// version_text must be "M.m.u" and say what version16 says (M << 10 | m << 4 | u).
static int version_text_ok(const char* t, uint16_t v16) {
    unsigned part[3] = {0, 0, 0};
    int k = 0, digits = 0;
    for (const char* p = t; *p; p++) {
        if (*p == '.') {
            if (digits == 0 || ++k > 2) {
                return 0;
            }
            digits = 0;
        } else if (*p >= '0' && *p <= '9' && digits < 2) {
            part[k] = part[k] * 10 + (unsigned) (*p - '0');
            digits++;
        } else {
            return 0;
        }
    }
    return k == 2 && digits > 0 && part[0] < 64 && part[1] < 64 && part[2] < 16 && ((part[0] << 10) | (part[1] << 4) | part[2]) == v16;
}

// manifest.txt in the format of make_dlp_child.sh (lower case hashes, upper case Title ID). A NULL hash gives zeros (to know the length). 0 when it does not fit.
static size_t manifest_text(const dlpc_job* j, const uint8_t* ha, const uint8_t* hp, char* out, size_t cap) {
    sb_t s = {out, cap, 0, 0};
    sb_str(&s, "version=");
    sb_str(&s, j->version_text);
    sb_str(&s, "\nversion16=");
    sb_dec(&s, j->version16);
    sb_str(&s, "\ntid=");
    sb_hex(&s, j->app_tid, 16, 1);
    sb_str(&s, "\n3dslink.cia=");
    sb_dec(&s, j->app_size);
    sb_ch(&s, ' ');
    for (int i = 0; i < 32; i++) {
        sb_hex(&s, ha ? ha[i] : 0, 2, 0);
    }
    sb_str(&s, "\nAzaharLink.3gx=");
    sb_dec(&s, j->plugin_size);
    sb_ch(&s, ' ');
    for (int i = 0; i < 32; i++) {
        sb_hex(&s, hp ? hp[i] : 0, 2, 0);
    }
    sb_ch(&s, '\n');
    if (s.over) {
        return 0;
    }
    out[s.len] = 0;
    return s.len;
}

// 0 = laid out; DLPC_E_TOOBIG = a size is past the limit (nothing is computed); DLPC_E_VERSION = the version text is not usable.
static dlpc_result make_layout(const dlpc_job* j, const model_info* mi, layout* L) {
    memset(L, 0, sizeof(*L));
    if (j->app_size > MAX_CHILD || j->plugin_size > MAX_CHILD) {
        return DLPC_E_TOOBIG;
    }
    if (j->version_text == NULL || strlen(j->version_text) > 16 || !version_text_ok(j->version_text, j->version16)) {
        return DLPC_E_VERSION;
    }
    char tmp[512];
    const size_t mlen = manifest_text(j, NULL, NULL, tmp, sizeof(tmp));
    if (mlen == 0) {
        return DLPC_E_VERSION;
    }
    // Everything below is at most about 2^27 (two inputs of 32 MiB at most), in 64 bits.
    uint64_t size[NFILES] = {j->app_size, j->plugin_size, mlen}, ent = 0;
    for (int f = 0; f < NFILES; f++) {
        L->fsize[f] = (uint32_t) size[f];
        L->fnlen[f] = (uint32_t) strlen(kNames[f]);
        L->fent[f] = (uint32_t) ent;
        ent += 0x20 + al(2ull * L->fnlen[f], 4); // parent, sibling, data offset, data size, next of the hash chain, name length, name in UTF-16
    }
    L->dh_o = 0x28;
    L->dh_s = 4 * NBUCKETS;
    L->dm_o = L->dh_o + L->dh_s;
    L->dm_s = 0x18; // the root directory: one entry
    L->fh_o = L->dm_o + L->dm_s;
    L->fh_s = 4 * NBUCKETS;
    L->fm_o = L->fh_o + L->fh_s;
    L->fm_s = (uint32_t) ent;
    L->fd_o = (uint32_t) al((uint64_t) L->fm_o + L->fm_s, 16);
    uint64_t at = 0;
    for (int f = 0; f < NFILES; f++) {
        L->fdata[f] = (uint32_t) at;
        at = al(at + size[f], 16); // the data of every file starts on 16 bytes
    }
    const uint64_t s3 = (uint64_t) L->fd_o + L->fdata[F_MANIFEST] + L->fsize[F_MANIFEST];
    if (s3 > MAX_CHILD) {
        return DLPC_E_TOOBIG;
    }
    L->s3 = (uint32_t) s3;
    L->nb3 = (uint32_t) al(s3, BLK) / BLK;
    L->s2 = 32 * L->nb3;
    L->nb2 = (uint32_t) al(L->s2, BLK) / BLK;
    L->s1 = 32 * L->nb2;
    L->nb1 = (uint32_t) al(L->s1, BLK) / BLK;
    L->mh = 32 * L->nb1;
    L->p3 = (uint32_t) al(0x60ull + L->mh, BLK);
    L->p1 = L->p3 + (uint32_t) al(L->s3, BLK);
    L->p2 = L->p1 + (uint32_t) al(L->s1, BLK);
    L->region = L->p2 + (uint32_t) al(L->s2, BLK);
    L->head = mi->head;
    L->meta = mi->meta;
    L->romfs_off = mi->romfs_off;
    const uint64_t ncch = (uint64_t) mi->romfs_off + L->region, file = (uint64_t) mi->head + L->region + mi->meta;
    if (file > MAX_CHILD) {
        return DLPC_E_TOOBIG;
    }
    L->ncch_size = (uint32_t) ncch;
    L->file_size = (uint32_t) file;
    return DLPC_OK;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// What the stages cost (the progress units, and the work by kind for the estimate)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
typedef struct {
    uint64_t total; // progress units: bytes that the passes of the stage go through
    dlpc_work w;
} plan_t;

static plan_t stage_plan(const layout* L, int emu, int stage) {
    plan_t p;
    memset(&p, 0, sizeof(p));
    const uint64_t A = L->fsize[F_APP], P = L->fsize[F_PLUGIN], M = L->fsize[F_MANIFEST], F = L->file_size, NC = L->ncch_size, R = L->region;
    const uint64_t tree = (uint64_t) L->nb3 * BLK + (uint64_t) L->nb2 * BLK + (uint64_t) L->nb1 * BLK, aes = emu ? 0 : R;
    switch (stage) {
    case DLPC_STAGE_READ_APP: // read + hash, then the hash chain of the TMD over the contents
        p.total = 2 * A;
        p.w.sd_read = A;
        p.w.sha = 2 * A;
        break;
    case DLPC_STAGE_READ_PLUGIN:
        p.total = P;
        p.w.sd_read = P;
        p.w.sha = P;
        break;
    case DLPC_STAGE_TREE:
        p.total = tree;
        p.w.sha = tree;
        break;
    case DLPC_STAGE_SEAL: // the encryption of the RomFS, then the hash of the stored bytes of the NCCH
        p.total = aes + NC;
        p.w.aes = aes;
        p.w.sha = NC + 0x200;
        break;
    case DLPC_STAGE_WRITE:
        p.total = F;
        p.w.sd_write = F;
        break;
    case DLPC_STAGE_VERIFY: // the read, the hash chain, the decryption, the three IVFC levels, the files of the manifest
        p.total = F + NC + aes + tree + A + P + M;
        p.w.sd_read = F;
        p.w.sha = NC + tree + A + P + M + 0x600;
        p.w.aes = emu ? 0 : R + 0x1000;
        break;
    default:
        break;
    }
    return p;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// The state of one job
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
typedef struct {
    const dlpc_job* job;
    char* err;
    size_t err_size;
    model_info mi;
    layout L;
    int emu;
    uint8_t* buf; // the whole child
    uint8_t app_sha[32], plugin_sha[32];
    char manifest[512];
    size_t manifest_len;
    char part[1040];
    const char* why; // the reason of the last CHK_BAD
    int stage;
    uint64_t total, done, since;
    uint32_t rk[44];
    uint8_t scratch[0x1000];
} ctx_t;

enum { CHK_OK = 0, CHK_BAD = 1, CHK_CANCEL = 2 };

// The first sentence stands (a later one would only describe what the first caused).
static dlpc_result fail(ctx_t* c, dlpc_result r, const char* msg) {
    if (c->err != NULL && c->err_size > 0 && c->err[0] == 0) {
        snprintf(c->err, c->err_size, "%s", msg);
    }
    return r;
}
static dlpc_result fail_why(ctx_t* c, dlpc_result r, const char* what, const char* why) {
    char msg[200];
    snprintf(msg, sizeof(msg), "%s (%s).", what, why != NULL ? why : "unknown");
    return fail(c, r, msg);
}

static int report(ctx_t* c) {
    return c->job->progress != NULL && c->job->progress(c->job->progress_user, c->stage, c->done, c->total) != 0;
}
// Each of the three returns 1 when the caller asked to cancel.
static int stage_begin(ctx_t* c, int stage) {
    const plan_t p = stage_plan(&c->L, c->emu, stage);
    c->stage = stage;
    c->total = p.total;
    c->done = 0;
    c->since = 0;
    return report(c);
}
static int step(ctx_t* c, uint64_t n) {
    c->done += n;
    if (c->done > c->total) {
        c->done = c->total;
    }
    c->since += n;
    if (c->since < CHUNK) {
        return 0;
    }
    c->since = 0;
    return report(c);
}
// The end of a pass: what the last steps added since the last call is reported now, so that no call is ever more than CHUNK of work after the one before.
static int flush(ctx_t* c) {
    if (c->since == 0) {
        return 0;
    }
    c->since = 0;
    return report(c);
}
static int stage_end(ctx_t* c) {
    c->done = c->total;
    c->since = 0;
    return report(c);
}

// SHA-256 of a block of memory in pieces of CHUNK, with progress. 1 = cancelled.
static int sha_pass(ctx_t* c, const uint8_t* p, uint64_t n, uint8_t out[32]) {
    sha256_ctx s;
    sha256_init(&s);
    while (n > 0) {
        const size_t k = n > CHUNK ? CHUNK : (size_t) n;
        sha256_update(&s, p, k);
        p += k;
        n -= k;
        if (step(c, k)) {
            return 1;
        }
    }
    if (flush(c)) {
        return 1;
    }
    sha256_final(&s, out);
    return 0;
}

// The NCCH counter of a section (1 exheader, 2 ExeFS, 3 RomFS): partition id big endian | section | seven zeros. The key is all zeros.
static void ncch_ctr(uint64_t pid, uint8_t section, uint8_t ctr[16]) {
    memset(ctr, 0, 16);
    put_be64(ctr, pid);
    ctr[8] = section;
}
// AES-128-CTR of a block of memory that starts at `offset` of its stream, in pieces of CHUNK, with progress. 1 = cancelled.
static int aes_pass(ctx_t* c, const uint8_t ctr[16], uint64_t offset, uint8_t* p, uint64_t n) {
    while (n > 0) {
        const size_t k = n > CHUNK ? CHUNK : (size_t) n;
        aes_ctr_crypt(c->rk, ctr, offset, p, k);
        p += k;
        offset += k;
        n -= k;
        if (step(c, k)) {
            return 1;
        }
    }
    return flush(c);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// The TMD chain: contents -> chunk records -> info records -> header (the rules of finalize_child.py and verify_cia.py)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
static int tmd_chain(ctx_t* c, const uint8_t* d, uint64_t len, const cia_lay* L) {
    uint8_t h[32];
    uint64_t pos = L->oc, sum = 0;
    for (uint32_t i = 0; i < L->n; i++) {
        const uint8_t* rec = d + L->cr + 0x30ull * i;
        const uint64_t size = be64(rec + 8);
        if (size > len || pos > len - size) {
            c->why = "a content of the TMD lies outside the file";
            return CHK_BAD;
        }
        if (sha_pass(c, d + pos, size, h)) {
            return CHK_CANCEL;
        }
        if (memcmp(h, rec + 0x10, 32) != 0) {
            c->why = "a content does not match its hash in the TMD";
            return CHK_BAD;
        }
        pos += al(size, 64);
        sum += al(size, 64);
    }
    if (sum != al(L->csz, 64) && sum != L->csz) {
        c->why = "the contents do not add up to the content size of the header";
        return CHK_BAD;
    }
    for (int k = 0; k < 64; k++) {
        const uint8_t* r = d + L->info + 0x24 * k;
        const uint32_t first = be16(r), count = be16(r + 2);
        if (count == 0) {
            continue;
        }
        if (first + count > L->n) {
            c->why = "an info record of the TMD points past the content records";
            return CHK_BAD;
        }
        sha256(d + L->cr + 0x30ull * first, 0x30ull * count, h);
        if (memcmp(h, r + 4, 32) != 0) {
            c->why = "an info record of the TMD does not match its content records";
            return CHK_BAD;
        }
    }
    sha256(d + L->info, 0x24 * 64, h);
    if (memcmp(h, d + L->hb + 0xA4, 32) != 0) {
        c->why = "the header of the TMD does not match its info records";
        return CHK_BAD;
    }
    return CHK_OK;
}

// The app's CIA: a CIA of app_tid and version16 whose contents match the TMD. The sizes follow the installer's ParseCia: a CIA with no meta may lack the
// padding of its last content (up to 63 bytes); one with a meta must be exact.
static int app_check(ctx_t* c, const uint8_t* d, uint64_t len) {
    cia_lay L;
    c->why = cia_layout(d, len, &L);
    if (c->why != NULL) {
        return CHK_BAD;
    }
    if (L.csz == 0 || L.csz > len) {
        c->why = "the content size of its header is wrong";
        return CHK_BAD;
    }
    const uint64_t end = L.oc + al(L.csz, 64) + L.metasz;
    if (L.metasz != 0 ? len != end : (len > end || end - len >= 64)) {
        c->why = "its size is not what its header adds up to";
        return CHK_BAD;
    }
    if (be64(d + L.hb + 0x4C) != c->job->app_tid) {
        c->why = "it is another title";
        return CHK_BAD;
    }
    if (be16(d + L.hb + 0x9C) != c->job->version16) {
        c->why = "it is another version";
        return CHK_BAD;
    }
    return tmd_chain(c, d, len, &L);
}

// A .3gx of this version: "3GX$" and, at offset 8, the version of the plugin (byte 11 major, 10 minor, 9 micro: check_plugin_pair.py, system.cpp).
static const char* plugin_check(const dlpc_job* j, const uint8_t* d, uint64_t len) {
    if (len < 16 || memcmp(d, "3GX$", 4) != 0) {
        return "it is not a 3GX plugin";
    }
    if (d[11] > 63 || d[10] > 63 || d[9] > 15 || ((((unsigned) d[11] << 10) | ((unsigned) d[10] << 4) | d[9]) != j->version16)) {
        return "it is the plugin of another version";
    }
    return NULL;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// The RomFS: level 3 (header, name hash tables, directory and file entries, the files), the IVFC levels and the IVFC header
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
static uint32_t name_bucket(uint32_t parent, const char* name) {
    uint32_t h = parent ^ 123456789u;
    for (; *name; name++) {
        h = (h >> 5) | (h << 27);
        h ^= (uint8_t) *name; // the UTF-16 unit of an ASCII character is the character
    }
    return h % NBUCKETS;
}

// The tables of level 3 (the files' bytes are put by the stages). `l3` is zero.
static void l3_tables(const layout* L, uint8_t* l3) {
    put_le32(l3 + 0, 0x28);
    put_le32(l3 + 4, L->dh_o);
    put_le32(l3 + 8, L->dh_s);
    put_le32(l3 + 12, L->dm_o);
    put_le32(l3 + 16, L->dm_s);
    put_le32(l3 + 20, L->fh_o);
    put_le32(l3 + 24, L->fh_s);
    put_le32(l3 + 28, L->fm_o);
    put_le32(l3 + 32, L->fm_s);
    put_le32(l3 + 36, L->fd_o);
    for (uint32_t b = 0; b < NBUCKETS; b++) {
        put_le32(l3 + L->dh_o + 4 * b, NONE32);
        put_le32(l3 + L->fh_o + 4 * b, NONE32);
    }
    put_le32(l3 + L->dh_o + 4 * name_bucket(0, ""), 0); // the root directory (entry 0, no name) is in its bucket
    uint8_t* de = l3 + L->dm_o;
    put_le32(de + 0, 0);          // parent
    put_le32(de + 4, NONE32);     // sibling
    put_le32(de + 8, NONE32);     // first child directory
    put_le32(de + 12, L->fent[0]); // first child file
    put_le32(de + 16, NONE32);    // next of the hash chain
    put_le32(de + 20, 0);         // name length
    for (int f = 0; f < NFILES; f++) {
        uint8_t* fe = l3 + L->fm_o + L->fent[f];
        const uint32_t b = name_bucket(0, kNames[f]);
        put_le32(fe + 0, 0);
        put_le32(fe + 4, f + 1 < NFILES ? L->fent[f + 1] : NONE32);
        put_le64(fe + 8, L->fdata[f]);
        put_le64(fe + 16, L->fsize[f]);
        put_le32(fe + 24, le32(l3 + L->fh_o + 4 * b)); // the new entry goes in front of its bucket's chain
        put_le32(l3 + L->fh_o + 4 * b, L->fent[f]);
        put_le32(fe + 28, 2 * L->fnlen[f]);
        for (uint32_t i = 0; i < L->fnlen[f]; i++) {
            fe[0x20 + 2 * i] = (uint8_t) kNames[f][i];
        }
    }
}

// The header of the RomFS region: "IVFC", the three levels (logical offsets: level 1 at 0, level 2 and 3 on block boundaries after it), the master hash comes at 0x60.
static void ivfc_header(const layout* L, uint8_t* rg) {
    const uint64_t lo[3] = {0, al(L->s1, BLK), al(al(L->s1, BLK) + L->s2, BLK)}, ls[3] = {L->s1, L->s2, L->s3};
    memcpy(rg, "IVFC", 4);
    put_le32(rg + 4, 0x10000);
    put_le32(rg + 8, L->mh);
    for (int k = 0; k < 3; k++) {
        put_le64(rg + 0xC + 0x18 * k, lo[k]);
        put_le64(rg + 0x14 + 0x18 * k, ls[k]);
        put_le32(rg + 0x1C + 0x18 * k, 12);
    }
    put_le32(rg + 0x54, 0x5C);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// The bytes of the head that the composer writes (everything else of the head is the model's, byte for byte): the ranges, ascending
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
static int patch_ranges(const model_info* mi, uint32_t off[72], uint32_t len[72]) {
    const cia_lay* L = &mi->L;
    int n = 0;
#define RANGE(o, l)                                                                                                                                            \
    do {                                                                                                                                                       \
        off[n] = (uint32_t) (o);                                                                                                                               \
        len[n] = (uint32_t) (l);                                                                                                                               \
        n++;                                                                                                                                                   \
    } while (0)
    RANGE(0x18, 8);               // content size of the CIA header
    RANGE(L->hb + 0xA4, 32);      // TMD header: hash of the info records
    for (int k = 0; k < 64; k++) { // info records: the hash of the chunk records they cover
        if (be16(mi->head_p + L->info + 0x24 * k + 2) != 0) {
            RANGE(L->info + 0x24ull * (uint32_t) k + 4, 32);
        }
    }
    RANGE(L->cr + 8, 0x28);       // chunk record: size and hash (one content)
    RANGE(L->oc + 0x104, 4);      // NCCH size
    RANGE(L->oc + 0x1B4, 4);      // RomFS size
    RANGE(L->oc + 0x1E0, 32);     // hash of the RomFS header
#undef RANGE
    return n;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// The stages
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
static uint8_t* region_of(ctx_t* c) {
    return c->buf + c->L.head;
}
static uint8_t* l3_of(ctx_t* c) {
    return region_of(c) + c->L.p3;
}

static dlpc_result read_payload(ctx_t* c, int which) {
    const dlpc_job* j = c->job;
    const layout* L = &c->L;
    const int app = which == F_APP;
    dlpc_read_fn rd = app ? j->read_app : j->read_plugin;
    void* user = app ? j->app_user : j->plugin_user;
    uint8_t* dst = l3_of(c) + L->fd_o + L->fdata[which];
    const uint64_t size = L->fsize[which];
    uint8_t* sha = app ? c->app_sha : c->plugin_sha;
    const dlpc_result rd_fail = app ? DLPC_E_READ_APP : DLPC_E_READ_PLUGIN, bad = app ? DLPC_E_APP_BAD : DLPC_E_PLUGIN_BAD;
    if (stage_begin(c, app ? DLPC_STAGE_READ_APP : DLPC_STAGE_READ_PLUGIN)) {
        return DLPC_E_CANCEL;
    }
    sha256_ctx s;
    sha256_init(&s);
    for (uint64_t off = 0; off < size;) {
        const size_t k = size - off > CHUNK ? CHUNK : (size_t) (size - off);
        if (rd(user, off, dst + off, k) != 0) {
            return fail(c, rd_fail, app ? "The app's installation file could not be read." : "The plugin file could not be read.");
        }
        sha256_update(&s, dst + off, k);
        off += k;
        if (step(c, k)) {
            return DLPC_E_CANCEL;
        }
    }
    if (flush(c)) {
        return DLPC_E_CANCEL;
    }
    sha256_final(&s, sha);
    if (app) {
        const int rc = app_check(c, dst, size);
        if (rc == CHK_CANCEL) {
            return DLPC_E_CANCEL;
        }
        if (rc == CHK_BAD) {
            return fail_why(c, bad, "The app's installation file is not valid for this version", c->why);
        }
    } else {
        const char* why = plugin_check(j, dst, size);
        if (why != NULL) {
            return fail_why(c, bad, "The plugin file is not valid for this version", why);
        }
    }
    return stage_end(c) ? DLPC_E_CANCEL : DLPC_OK;
}

static dlpc_result build_tree(ctx_t* c) {
    const dlpc_job* j = c->job;
    const layout* L = &c->L;
    uint8_t* rg = region_of(c);
    uint8_t* l3 = l3_of(c);
    if (stage_begin(c, DLPC_STAGE_TREE)) {
        return DLPC_E_CANCEL;
    }
    c->manifest_len = manifest_text(j, c->app_sha, c->plugin_sha, c->manifest, sizeof(c->manifest));
    if (c->manifest_len != L->fsize[F_MANIFEST]) {
        return fail(c, DLPC_E_VERSION, "The manifest does not have the size that was planned.");
    }
    memcpy(l3 + L->fd_o + L->fdata[F_MANIFEST], c->manifest, c->manifest_len);
    l3_tables(L, l3);
    // level 2 = hashes of the blocks of level 3, level 1 = of level 2, the master hash = of level 1 (the blocks end in zero padding: the buffer is zeroed)
    for (uint32_t i = 0; i < L->nb3; i++) {
        sha256(l3 + (uint64_t) i * BLK, BLK, rg + L->p2 + 32 * i);
        if (step(c, BLK)) {
            return DLPC_E_CANCEL;
        }
    }
    for (uint32_t i = 0; i < L->nb2; i++) {
        sha256(rg + L->p2 + (uint64_t) i * BLK, BLK, rg + L->p1 + 32 * i);
        if (step(c, BLK)) {
            return DLPC_E_CANCEL;
        }
    }
    for (uint32_t i = 0; i < L->nb1; i++) {
        sha256(rg + L->p1 + (uint64_t) i * BLK, BLK, rg + 0x60 + 32 * i);
        if (step(c, BLK)) {
            return DLPC_E_CANCEL;
        }
    }
    if (flush(c)) {
        return DLPC_E_CANCEL;
    }
    ivfc_header(L, rg);
    return stage_end(c) ? DLPC_E_CANCEL : DLPC_OK;
}

static dlpc_result seal(ctx_t* c) {
    const layout* L = &c->L;
    const model_info* mi = &c->mi;
    uint8_t* d = c->buf;
    uint8_t* rg = region_of(c);
    const cia_lay* M = &mi->L;
    uint8_t* nc = d + M->oc;
    uint8_t h[32];
    if (stage_begin(c, DLPC_STAGE_SEAL)) {
        return DLPC_E_CANCEL;
    }
    put_le32(nc + 0x104, L->ncch_size / 0x200);
    put_le32(nc + 0x1B4, L->region / 0x200);
    sha256(rg, mi->hr, nc + 0x1E0); // on the PLAIN header of the RomFS
    put_le64(d + 0x18, L->ncch_size);
    if (!c->emu) {
        uint8_t ctr[16], zero[16];
        memset(zero, 0, sizeof(zero));
        aes_set_key(c->rk, zero);
        ncch_ctr(mi->tid, 3, ctr);
        if (aes_pass(c, ctr, 0, rg, L->region)) {
            return DLPC_E_CANCEL;
        }
    }
    // the TMD chain on the stored bytes
    put_be64(d + M->cr + 8, L->ncch_size);
    if (sha_pass(c, nc, L->ncch_size, d + M->cr + 0x10)) {
        return DLPC_E_CANCEL;
    }
    for (int k = 0; k < 64; k++) {
        uint8_t* r = d + M->info + 0x24 * k;
        const uint32_t first = be16(r), count = be16(r + 2);
        if (count != 0) {
            sha256(d + M->cr + 0x30ull * first, 0x30ull * count, h);
            memcpy(r + 4, h, 32);
        }
    }
    sha256(d + M->info, 0x24 * 64, d + M->hb + 0xA4);
    return stage_end(c) ? DLPC_E_CANCEL : DLPC_OK;
}

static dlpc_result write_part(ctx_t* c) {
    const layout* L = &c->L;
    if (stage_begin(c, DLPC_STAGE_WRITE)) {
        return DLPC_E_CANCEL;
    }
    FILE* f = fopen(c->part, "wb");
    if (f == NULL) {
        return fail(c, DLPC_E_WRITE, "The file could not be created on the SD card.");
    }
    for (uint64_t off = 0; off < L->file_size;) {
        const size_t k = L->file_size - off > CHUNK ? CHUNK : (size_t) (L->file_size - off);
        errno = 0;
        if (fwrite(c->buf + off, 1, k, f) != k) {
            const int full = errno == ENOSPC;
            fclose(f);
            return fail(c, DLPC_E_WRITE, full ? "The SD card is full." : "The SD card refused the file.");
        }
        off += k;
        if (step(c, k)) {
            fclose(f);
            return DLPC_E_CANCEL;
        }
    }
    if (flush(c)) {
        fclose(f);
        return DLPC_E_CANCEL;
    }
    errno = 0;
    int ok = fflush(f) == 0 && ferror(f) == 0;
    int e = errno;
    if (fclose(f) != 0) {
        ok = 0;
        e = e != 0 ? e : errno;
    }
    if (!ok) {
        return fail(c, DLPC_E_WRITE, e == ENOSPC ? "The SD card is full." : "The SD card refused the file.");
    }
    return stage_end(c) ? DLPC_E_CANCEL : DLPC_OK;
}

// --- the read-back check ---------------------------------------------------------------------------------------------------------------------------------

// All the little checks of the file that was read back. The block `c->buf` holds the file as read; the RomFS is decrypted in place at the end.
#define VFAIL(msg)                                                                                                                                             \
    do {                                                                                                                                                       \
        c->why = (msg);                                                                                                                                        \
        return CHK_BAD;                                                                                                                                        \
    } while (0)

// The directory tables of level 3 and the three files in it, parsed from the bytes (not from the plan): every entry in its bounds, the names, the sizes
// and offsets the plan says, every file reachable through the hash chain of its name.
static int verify_l3(ctx_t* c, const uint8_t* l3) {
    const layout* L = &c->L;
    if (le32(l3) != 0x28 || le32(l3 + 4) != L->dh_o || le32(l3 + 8) != L->dh_s || le32(l3 + 12) != L->dm_o || le32(l3 + 16) != L->dm_s || le32(l3 + 20) != L->fh_o ||
        le32(l3 + 24) != L->fh_s || le32(l3 + 28) != L->fm_o || le32(l3 + 32) != L->fm_s || le32(l3 + 36) != L->fd_o) {
        VFAIL("the header of the file tables is not what was planned");
    }
    const uint8_t* de = l3 + L->dm_o;
    if (le32(de) != 0 || le32(de + 4) != NONE32 || le32(de + 8) != NONE32 || le32(de + 16) != NONE32 || le32(de + 20) != 0) {
        VFAIL("the root directory entry is wrong");
    }
    if (le32(l3 + L->dh_o + 4 * name_bucket(0, "")) != 0) {
        VFAIL("the root directory is not in its hash bucket");
    }
    int seen = 0;
    uint32_t fo = le32(de + 12);
    for (int guard = 0; fo != NONE32 && guard <= NFILES; guard++) {
        if ((uint64_t) fo + 0x20 > L->fm_s) {
            VFAIL("a file entry lies outside its table");
        }
        const uint8_t* fe = l3 + L->fm_o + fo;
        const uint32_t nlen = le32(fe + 0x1C);
        if (nlen == 0 || nlen > 64 || (nlen & 1) || (uint64_t) fo + 0x20 + al(nlen, 4) > L->fm_s) {
            VFAIL("a file name lies outside its table");
        }
        char name[40];
        const uint32_t chars = nlen / 2;
        if (chars >= sizeof(name)) {
            VFAIL("a file name is too long");
        }
        for (uint32_t i = 0; i < chars; i++) {
            if (fe[0x21 + 2 * i] != 0 || fe[0x20 + 2 * i] == 0 || fe[0x20 + 2 * i] >= 0x80) {
                VFAIL("a file name is not plain ASCII");
            }
            name[i] = (char) fe[0x20 + 2 * i];
        }
        name[chars] = 0;
        int idx = -1;
        for (int f = 0; f < NFILES; f++) {
            if (strcmp(name, kNames[f]) == 0) {
                idx = f;
            }
        }
        if (idx < 0 || (seen & (1 << idx))) {
            VFAIL("an unexpected or repeated file in the RomFS");
        }
        seen |= 1 << idx;
        if (fo != L->fent[idx] || le32(fe) != 0 || le64(fe + 8) != L->fdata[idx] || le64(fe + 16) != L->fsize[idx]) {
            VFAIL("a file entry is not what was planned");
        }
        if ((uint64_t) L->fd_o + le64(fe + 8) + le64(fe + 16) > L->s3) {
            VFAIL("a file lies outside level 3");
        }
        uint32_t at = le32(l3 + L->fh_o + 4 * name_bucket(0, name));
        int hops = 0;
        while (at != fo && at != NONE32 && hops++ <= NFILES) {
            if ((uint64_t) at + 0x20 > L->fm_s) {
                VFAIL("a hash chain points outside the file table");
            }
            at = le32(l3 + L->fm_o + at + 24);
        }
        if (at != fo) {
            VFAIL("a file cannot be found through the hash table of names");
        }
        fo = le32(fe + 4);
    }
    if (fo != NONE32 || seen != (1 << NFILES) - 1) {
        VFAIL("the RomFS does not hold exactly the three files");
    }
    return CHK_OK;
}

// A block of memory that must be zero (the padding of the levels).
static int is_zero(const uint8_t* p, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        if (p[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static int verify_buffer(ctx_t* c) {
    const layout* L = &c->L;
    const model_info* mi = &c->mi;
    const dlpc_job* j = c->job;
    uint8_t* d = c->buf;
    const uint64_t F = L->file_size;
    cia_lay V;
    uint8_t h[32];
    int rc;

    // --- the CIA: the same place for everything as in the model, the head equal to the model except for the bytes that were composed
    c->why = cia_layout(d, F, &V);
    if (c->why != NULL) {
        return CHK_BAD;
    }
    const cia_lay* M = &mi->L;
    if (V.oc != M->oc || V.om != M->om || V.hb != M->hb || V.tb != M->tb || V.tmdsz != M->tmdsz || V.metasz != M->metasz || V.n != 1 || V.csz != L->ncch_size) {
        VFAIL("the CIA header is not that of the model");
    }
    if (F != V.oc + al(V.csz, 64) + V.metasz) {
        VFAIL("the size of the file is not what its header adds up to");
    }
    uint32_t po[72], pl[72];
    const int np = patch_ranges(mi, po, pl);
    uint32_t pos = 0;
    for (int k = 0; k < np; k++) {
        if (memcmp(d + pos, mi->head_p + pos, po[k] - pos) != 0) {
            VFAIL("the head of the file differs from the model");
        }
        pos = po[k] + pl[k];
    }
    if (memcmp(d + pos, mi->head_p + pos, mi->head - pos) != 0) {
        VFAIL("the head of the file differs from the model");
    }
    if (memcmp(d + L->head + L->region, mi->meta_p, mi->meta) != 0) {
        VFAIL("the meta block differs from the model");
    }
    // --- the conditions of dlph_open and of the installer
    const uint8_t* nc = d + V.oc;
    if (F <= 0x2040 || F > MAX_CHILD || le32(d + 0x1C) != 0) {
        VFAIL("the size of the child is not accepted by Download Play");
    }
    if (be64(d + V.hb + 0x4C) != mi->tid || (mi->tid >> 32) != 0x00040001u || (be16(d + V.cr + 6) & 1) != 0) {
        VFAIL("the Title ID or the content type is not that of a child");
    }
    const uint64_t meta_off = V.oc + al(V.csz, 64); // dlph_open: the SMDH is at meta + 0x400 and all of it (0x24C0 + 0x1200 after its start) is in the file
    if (meta_off + 0x400 + 0x24C0 + 0x1200 > F || memcmp(d + meta_off + 0x400, "SMDH", 4) != 0) {
        VFAIL("there is no usable SMDH in the meta block");
    }
    if (memcmp(nc + 0x100, "NCCH", 4) != 0 || (uint64_t) le32(nc + 0x104) * 0x200 != L->ncch_size || le64(nc + 0x108) != mi->tid || le64(nc + 0x118) != mi->tid ||
        (uint64_t) le32(nc + 0x1B0) * 0x200 != L->romfs_off || (uint64_t) le32(nc + 0x1B4) * 0x200 != L->region) {
        VFAIL("the NCCH header does not describe this file");
    }
    if (be64(d + V.cr + 8) != L->ncch_size) {
        VFAIL("the TMD does not say the size of the NCCH");
    }
    const uint8_t fl7 = nc[0x18F];
    if (c->emu ? (fl7 & 4) == 0 : fl7 != 0x01) {
        VFAIL("the NCCH flags are not those of this variant");
    }
    // --- the TMD chain over the bytes as stored
    rc = tmd_chain(c, d, F, &V);
    if (rc != CHK_OK) {
        return rc;
    }
    // --- the exheader and the first part of the ExeFS (decrypted in a copy)
    uint8_t ctr[16], zero[16];
    memset(zero, 0, sizeof(zero));
    aes_set_key(c->rk, zero);
    {
        const uint32_t exsize = le32(nc + 0x180), ehr = le32(nc + 0x1A8) * 0x200;
        const uint8_t* ex = nc + 0x200;
        const uint8_t* fx = nc + (uint64_t) le32(nc + 0x1A0) * 0x200;
        if (exsize != 0x400 || ehr == 0 || ehr > sizeof(c->scratch)) {
            VFAIL("the exheader or the ExeFS has an unexpected size");
        }
        memcpy(c->scratch, ex, exsize);
        if (!c->emu) {
            ncch_ctr(mi->tid, 1, ctr);
            aes_ctr_crypt(c->rk, ctr, 0, c->scratch, exsize);
        }
        sha256(c->scratch, exsize, h);
        if (memcmp(h, nc + 0x160, 32) != 0) {
            VFAIL("the hash of the exheader does not match");
        }
        memcpy(c->scratch, fx, ehr);
        if (!c->emu) {
            ncch_ctr(mi->tid, 2, ctr);
            aes_ctr_crypt(c->rk, ctr, 0, c->scratch, ehr);
        }
        sha256(c->scratch, ehr, h);
        if (memcmp(h, nc + 0x1C0, 32) != 0) {
            VFAIL("the hash of the ExeFS does not match");
        }
    }
    // --- the RomFS, decrypted in place
    uint8_t* rg = region_of(c);
    if (!c->emu) {
        ncch_ctr(mi->tid, 3, ctr);
        if (aes_pass(c, ctr, 0, rg, L->region)) {
            return CHK_CANCEL;
        }
    }
    sha256(rg, mi->hr, h);
    if (memcmp(h, nc + 0x1E0, 32) != 0) {
        VFAIL("the hash of the RomFS header does not match (decryption)");
    }
    if (memcmp(rg, "IVFC", 4) != 0 || le32(rg + 4) != 0x10000 || le32(rg + 8) != L->mh || le32(rg + 0x54) != 0x5C) {
        VFAIL("the IVFC header is wrong");
    }
    {
        const uint64_t lo[3] = {0, al(L->s1, BLK), al(al(L->s1, BLK) + L->s2, BLK)}, ls[3] = {L->s1, L->s2, L->s3};
        for (int k = 0; k < 3; k++) {
            if (le64(rg + 0xC + 0x18 * k) != lo[k] || le64(rg + 0x14 + 0x18 * k) != ls[k] || le32(rg + 0x1C + 0x18 * k) != 12) {
                VFAIL("an IVFC level is not what was planned");
            }
        }
    }
    if (!is_zero(rg + 0x60 + L->mh, L->p3 - (0x60 + L->mh)) || !is_zero(rg + L->p3 + L->s3, al(L->s3, BLK) - L->s3) ||
        !is_zero(rg + L->p1 + L->s1, al(L->s1, BLK) - L->s1) || !is_zero(rg + L->p2 + L->s2, al(L->s2, BLK) - L->s2)) {
        VFAIL("the padding of the RomFS is not zero");
    }
    for (uint32_t i = 0; i < L->nb3; i++) {
        sha256(rg + L->p3 + (uint64_t) i * BLK, BLK, h);
        if (memcmp(h, rg + L->p2 + 32 * i, 32) != 0) {
            VFAIL("IVFC level 3 does not match level 2");
        }
        if (step(c, BLK)) {
            return CHK_CANCEL;
        }
    }
    if (flush(c)) {
        return CHK_CANCEL;
    }
    for (uint32_t i = 0; i < L->nb2; i++) {
        sha256(rg + L->p2 + (uint64_t) i * BLK, BLK, h);
        if (memcmp(h, rg + L->p1 + 32 * i, 32) != 0) {
            VFAIL("IVFC level 2 does not match level 1");
        }
        if (step(c, BLK)) {
            return CHK_CANCEL;
        }
    }
    for (uint32_t i = 0; i < L->nb1; i++) {
        sha256(rg + L->p1 + (uint64_t) i * BLK, BLK, h);
        if (memcmp(h, rg + 0x60 + 32 * i, 32) != 0) {
            VFAIL("IVFC level 1 does not match the master hash");
        }
        if (step(c, BLK)) {
            return CHK_CANCEL;
        }
    }
    if (flush(c)) {
        return CHK_CANCEL;
    }
    // --- the file tables, the manifest, the files
    const uint8_t* l3 = rg + L->p3;
    rc = verify_l3(c, l3);
    if (rc != CHK_OK) {
        return rc;
    }
    char want[512];
    if (manifest_text(j, c->app_sha, c->plugin_sha, want, sizeof(want)) != L->fsize[F_MANIFEST] ||
        memcmp(want, l3 + L->fd_o + L->fdata[F_MANIFEST], L->fsize[F_MANIFEST]) != 0) {
        VFAIL("manifest.txt is not what the files say");
    }
    if (sha_pass(c, l3 + L->fd_o + L->fdata[F_APP], L->fsize[F_APP], h)) {
        return CHK_CANCEL;
    }
    if (memcmp(h, c->app_sha, 32) != 0) {
        VFAIL("the app's CIA in the RomFS is not the one that was read");
    }
    if (sha_pass(c, l3 + L->fd_o + L->fdata[F_PLUGIN], L->fsize[F_PLUGIN], h)) {
        return CHK_CANCEL;
    }
    if (memcmp(h, c->plugin_sha, 32) != 0) {
        VFAIL("the plugin in the RomFS is not the one that was read");
    }
    return CHK_OK;
}

static dlpc_result verify_part(ctx_t* c) {
    const layout* L = &c->L;
    if (stage_begin(c, DLPC_STAGE_VERIFY)) {
        return DLPC_E_CANCEL;
    }
    FILE* f = fopen(c->part, "rb");
    if (f == NULL) {
        return fail(c, DLPC_E_VERIFY, "The file could not be read back from the SD card.");
    }
    memset(c->buf, 0xA5, L->file_size); // nothing of what was composed may stand in for what the card gives back
    for (uint64_t off = 0; off < L->file_size;) {
        const size_t k = L->file_size - off > CHUNK ? CHUNK : (size_t) (L->file_size - off);
        if (fread(c->buf + off, 1, k, f) != k) {
            fclose(f);
            return fail(c, DLPC_E_VERIFY, "The file read back from the SD card is shorter than what was written.");
        }
        off += k;
        if (step(c, k)) {
            fclose(f);
            return DLPC_E_CANCEL;
        }
    }
    if (flush(c)) {
        fclose(f);
        return DLPC_E_CANCEL;
    }
    uint8_t extra;
    const int longer = fread(&extra, 1, 1, f) != 0;
    fclose(f);
    if (longer) {
        return fail(c, DLPC_E_VERIFY, "The file read back from the SD card is longer than what was written.");
    }
    const int rc = verify_buffer(c);
    if (rc == CHK_CANCEL) {
        return DLPC_E_CANCEL;
    }
    if (rc == CHK_BAD) {
        return fail_why(c, DLPC_E_VERIFY, "The installer file on the SD card did not pass the check", c->why);
    }
    return stage_end(c) ? DLPC_E_CANCEL : DLPC_OK;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// The job
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
static dlpc_result run(ctx_t* c) {
    const dlpc_job* j = c->job;
    dlpc_result r;
    if (j->model == NULL || j->read_app == NULL || j->read_plugin == NULL || j->out_path == NULL || j->out_path[0] == 0 || j->version_text == NULL) {
        return fail(c, DLPC_E_MODEL, "The job is incomplete.");
    }
    if (strlen(j->out_path) > 900) {
        return fail(c, DLPC_E_WRITE, "The path of the installer file is too long.");
    }
    snprintf(c->part, sizeof(c->part), "%s.part", j->out_path);
    remove(j->out_path); // a failure leaves no child (see dlpc.h)
    remove(c->part);

    const char* why = model_parse(j->model, j->model_size, 1, &c->mi);
    if (why != NULL) {
        return fail_why(c, DLPC_E_MODEL, "The installer inside the app is damaged", why);
    }
    c->emu = j->emu_variant ? 1 : 0;
    if (c->mi.emu != c->emu) {
        return fail(c, DLPC_E_MODEL, "The installer inside the app is not the one for this kind of child.");
    }
    if (c->mi.version != j->version16) {
        return fail(c, DLPC_E_VERSION, "The installer inside the app is from another version of 3DS Link.");
    }
    if (j->app_tid == 0) {
        return fail(c, DLPC_E_APP_BAD, "The title of the app is not set.");
    }
    if (j->app_size < 0x2040) {
        return fail(c, DLPC_E_APP_BAD, "The app's installation file is too short to be a CIA.");
    }
    if (j->plugin_size < 16) {
        return fail(c, DLPC_E_PLUGIN_BAD, "The plugin file is too short to be a plugin.");
    }
    r = make_layout(j, &c->mi, &c->L);
    if (r == DLPC_E_TOOBIG) {
        return fail(c, r, "The installer would pass 32 MB: the app or the plugin is too big.");
    }
    if (r != DLPC_OK) {
        return fail(c, r, "The version of 3DS Link is not written as major.minor.micro.");
    }
    c->buf = (uint8_t*) calloc(1, c->L.file_size);
    if (c->buf == NULL) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Not enough memory to build the installer (%u MB needed).", (unsigned) ((c->L.file_size + 0xFFFFF) >> 20));
        return fail(c, DLPC_E_NOMEM, msg);
    }
    aes_init();
    memcpy(c->buf, c->mi.head_p, c->mi.head);
    memcpy(c->buf + c->L.head + c->L.region, c->mi.meta_p, c->mi.meta);

    if ((r = read_payload(c, F_APP)) != DLPC_OK || (r = read_payload(c, F_PLUGIN)) != DLPC_OK || (r = build_tree(c)) != DLPC_OK || (r = seal(c)) != DLPC_OK ||
        (r = write_part(c)) != DLPC_OK || (r = verify_part(c)) != DLPC_OK) {
        return r;
    }
    remove(j->out_path); // (nothing is there since the job started; rename() does not replace a file on every file system)
    if (rename(c->part, j->out_path) != 0) {
        return fail(c, DLPC_E_WRITE, "The installer file could not be put in its place on the SD card.");
    }
    return DLPC_OK;
}

dlpc_result dlpc_compose(const dlpc_job* job, char* err, size_t err_size) {
    if (err != NULL && err_size > 0) {
        err[0] = 0;
    }
    if (job == NULL) {
        if (err != NULL && err_size > 0) {
            snprintf(err, err_size, "No job.");
        }
        return DLPC_E_MODEL;
    }
    ctx_t* c = (ctx_t*) calloc(1, sizeof(*c));
    if (c == NULL) {
        if (err != NULL && err_size > 0) {
            snprintf(err, err_size, "Not enough memory to build the installer.");
        }
        return DLPC_E_NOMEM;
    }
    c->job = job;
    c->err = err;
    c->err_size = err_size;
    const dlpc_result r = run(c);
    if (r != DLPC_OK) {
        if (c->part[0] != 0) {
            remove(c->part);
        }
        if (job->out_path != NULL && job->out_path[0] != 0) {
            remove(job->out_path);
        }
        if (err != NULL && err_size > 0 && err[0] == 0) {
            snprintf(err, err_size, "%s", dlpc_result_text(r));
        }
    }
    free(c->buf);
    free(c);
    return r;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// Questions that need no I/O
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// 1 = the job is laid out (mi and L are filled); 0 = it is not, and *toobig says whether the reason is the size. The sizes are bounded by make_layout before
// anything is added, so whatever the caller passes the sums cannot wrap. (The SHA-256 of the model is not checked here: dlpc_compose does that.)
static int lay_for_query(const dlpc_job* job, model_info* mi, layout* L, int* toobig) {
    *toobig = 0;
    if (job == NULL || model_parse(job->model, job->model_size, 0, mi) != NULL) {
        return 0;
    }
    const dlpc_result r = make_layout(job, mi, L);
    *toobig = r == DLPC_E_TOOBIG;
    return r == DLPC_OK;
}

uint64_t dlpc_output_size(const dlpc_job* job) {
    model_info mi;
    layout L;
    int toobig;
    if (lay_for_query(job, &mi, &L, &toobig)) {
        return L.file_size;
    }
    return toobig ? MAX_CHILD + 1ull : 0; // "more than the limit" has no exact size
}

size_t dlpc_memory_needed(const dlpc_job* job) {
    const uint64_t out = dlpc_output_size(job);
    return out == 0 ? 0 : (size_t) (out + sizeof(ctx_t));
}

dlpc_work dlpc_stage_work(const dlpc_job* job, int stage) {
    model_info mi;
    layout L;
    dlpc_work none;
    int toobig;
    memset(&none, 0, sizeof(none));
    if (!lay_for_query(job, &mi, &L, &toobig)) {
        return none;
    }
    return stage_plan(&L, job->emu_variant ? 1 : 0, stage).w;
}

const char* dlpc_result_text(dlpc_result r) {
    switch (r) {
    case DLPC_OK:
        return "Done.";
    case DLPC_E_MODEL:
        return "The installer inside the app is damaged.";
    case DLPC_E_VERSION:
        return "The installer inside the app is from another version.";
    case DLPC_E_READ_APP:
        return "The app's installation file could not be read.";
    case DLPC_E_READ_PLUGIN:
        return "The plugin file could not be read.";
    case DLPC_E_APP_BAD:
        return "The app's installation file is not valid for this version.";
    case DLPC_E_PLUGIN_BAD:
        return "The plugin file is not valid for this version.";
    case DLPC_E_NOMEM:
        return "Not enough memory.";
    case DLPC_E_TOOBIG:
        return "The installer would be too big.";
    case DLPC_E_WRITE:
        return "The SD card refused the file.";
    case DLPC_E_VERIFY:
        return "The installer file did not pass the check.";
    case DLPC_E_CANCEL:
        return "Cancelled.";
    default:
        return "Something went wrong.";
    }
}

const char* dlpc_stage_text(int stage) {
    switch (stage) {
    case DLPC_STAGE_READ_APP:
        return "Reading the app";
    case DLPC_STAGE_READ_PLUGIN:
        return "Reading the plugin";
    case DLPC_STAGE_TREE:
        return "Building the package";
    case DLPC_STAGE_SEAL:
        return "Encrypting and signing the package";
    case DLPC_STAGE_WRITE:
        return "Writing the package to the card";
    case DLPC_STAGE_VERIFY:
        return "Checking the package";
    default:
        return "";
    }
}
