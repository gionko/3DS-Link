#include "dlph_wire.h"
#include <string.h>

static void be16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { be16(p, v >> 16); be16(p + 2, v & 0xFFFF); }
static void be64(uint8_t *p, uint64_t v) { be32(p, (uint32_t)(v >> 32)); be32(p + 4, (uint32_t)v); }
static uint32_t rbe32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static uint32_t frame_sum(const uint8_t *f, size_t len)
{
    uint32_t sum = 0;
    size_t i = 0;
    for (; i + 4 <= len; i += 4) {
        uint32_t w = rbe32(f + i);
        if (i == 8) w = 0;                      // the checksum field counts as zero
        sum += w;
    }
    if (i < len) {                              // 1..3 trailing bytes, big-endian, zero padded
        uint32_t w = 0;
        for (size_t k = 0; i + k < len; k++) w |= (uint32_t)f[i + k] << (24 - 8 * k);
        sum += w;
    }
    return sum;
}

uint32_t dlph_checksum(const uint8_t seed[4], const uint8_t *f, size_t len)
{
    uint32_t h = frame_sum(f, len), key = rbe32(seed);
    unsigned rot = (seed[2] & 0xF) + 4, cnt = (seed[3] & 7) + 2;
    for (unsigned r = 0; r < cnt; r++) h = ((h >> rot) | (h << rot)) ^ key;     // NOT a rotation: same shift both ways
    return h;
}
void dlph_seal(const uint8_t seed[4], uint8_t *f, size_t len) { be32(f + 8, dlph_checksum(seed, f, len)); }
int  dlph_check(const uint8_t seed[4], const uint8_t *f, size_t len) { return len >= DLPH_HDR && dlph_checksum(seed, f, len) == rbe32(f + 8); }

// header: type, 0x02, 00 00, size BE, 02 00 (must_be_2 little endian), checksum (later), index, b13, b14, b15
static void hdr(uint8_t *o, uint8_t type, size_t size, uint8_t idx, uint8_t b13, uint8_t b14, uint8_t b15)
{
    memset(o, 0, size);
    o[0] = type; o[1] = 0x02;
    be16(o + 4, (uint32_t)size);
    o[6] = 0x02;
    o[12] = idx; o[13] = b13; o[14] = b14; o[15] = b15;
}

uint64_t dlph_cver_tid(char region)
{
    static const struct { char r; uint32_t lo; } T[] = { { 'J', 0x17202 }, { 'U', 0x17302 }, { 'E', 0x17102 }, { 'C', 0x17402 }, { 'K', 0x17502 }, { 'T', 0x17602 } };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (T[i].r == region) return 0x000400DB00000000ULL | T[i].lo;
    return 0;
}
uint16_t dlph_cver_version(unsigned major, unsigned minor, unsigned build) { return (uint16_t)((major << 10) | (minor << 4) | build); }

int dlph_beacon_set_host_cver(dlph_beacon_desc *d, char region, unsigned major, unsigned minor, unsigned build)
{
    uint64_t tid = dlph_cver_tid(region);
    if (!tid) return -1;
    d->cver_version = dlph_cver_version(major, minor, build);
    d->n_sys = 2;
    d->sys_h[0] = 1; d->sys_h[1] = 2; d->sys_h[2] = 0; d->sys_h[3] = 1;
    d->sys[0].tid = d->sys[1].tid = tid;
    d->sys[0].version = d->sys[1].version = d->cver_version;
    d->sys[0].size = d->sys[1].size = 0;
    return 0;
}

void dlph_beacon_set_no_cver(dlph_beacon_desc *d)
{
    d->cver_version = 0;
    d->n_sys = -1;
    memset(d->sys_h, 0, sizeof d->sys_h);
}

void dlph_beacon_zelda_constants(dlph_beacon_desc *d)
{
    d->k1a = 0x01; d->k1b = 0x02;
    d->chunk_size = DLPH_CHUNK_SIZE;
    d->k21 = 0x02;
    d->k24[0] = 1; d->k24[1] = d->k24[2] = d->k24[3] = 0;
    memset(d->k28, 0, sizeof d->k28);
    static const uint8_t t[8] = { 0x0B, 0x11, 0x00, 0x00, 0x45, 0x0B, 0x11, 0x00 };
    memcpy(d->k2f8, t, 8);
    dlph_beacon_set_host_cver(d, 'E', 11, 17, 0);           // the capture was taken on console A: 11.17.0-50E
}

static void swap16(uint8_t *dst, const uint8_t *src_le, size_t bytes)       // SMDH is UTF-16LE / RGB565LE, the wire is big-endian
{
    for (size_t i = 0; i + 1 < bytes; i += 2) { dst[i] = src_le[i + 1]; dst[i + 1] = src_le[i]; }
}

int dlph_beacon_frames(const dlph_beacon_desc *d, const uint8_t seed[4], uint8_t out[5][DLPH_BC4_LEN], size_t len[5])
{
    const int total = d->n_sys >= 0 ? 5 : 4;

    uint8_t *f = out[0];
    hdr(f, DLPH_T_BEACON, DLPH_BC0_LEN, 0, (uint8_t)total, 0, 0);
    be64(f + 0x10, d->tid);
    be16(f + 0x18, d->version);
    f[0x1A] = d->k1a; f[0x1B] = d->k1b;
    be32(f + 0x1C, d->chunk_size);
    f[0x20] = d->max_nodes; f[0x21] = d->k21;
    be16(f + 0x22, d->cver_version);
    memcpy(f + 0x24, d->k24, 4);
    memcpy(f + 0x28, d->k28, 16);
    be32(f + 0x38, d->transfer_size);
    be32(f + 0x3C, d->required_size);
    swap16(f + 0x40, d->name_le, DLPH_NAME_BYTES);
    swap16(f + 0xC0, d->desc_le, DLPH_DESC_BYTES);
    swap16(f + 0x1C0, d->icon_le, 312);                     // the icon starts here: 0x9C u16 (frames 1..3 carry the other 3 x 1432 bytes)
    memcpy(f + 0x2F8, d->k2f8, 8);
    len[0] = DLPH_BC0_LEN;
    for (int k = 1; k <= 3; k++) {
        hdr(out[k], DLPH_T_BEACON, DLPH_BCI_LEN, (uint8_t)k, (uint8_t)total, 0, 0);
        swap16(out[k] + DLPH_HDR, d->icon_le + 312 + (k - 1) * 1432, 1432);
        len[k] = DLPH_BCI_LEN;
    }
    if (total == 5) {
        f = out[4];
        hdr(f, DLPH_T_BEACON, DLPH_BC4_LEN, 4, 5, 0, 0);
        for (int i = 0; i < 4; i++) be16(f + 0x10 + 2 * i, d->sys_h[i]);
        for (int i = 0; i < d->n_sys && i < (int)DLPH_MAX_SYS; i++) {
            uint8_t *e = f + 0x18 + 16 * i;
            be64(e, d->sys[i].tid); be16(e + 8, d->sys[i].version); be32(e + 12, d->sys[i].size);
        }
        len[4] = DLPH_BC4_LEN;
    }
    for (int k = 0; k < total; k++) dlph_seal(seed, out[k], len[k]);
    return total;
}

size_t dlph_pk_auth(uint8_t *o, const uint8_t seed[4])
{
    hdr(o, DLPH_T_AUTH, 0x14, 1, 0, 0, 0);
    dlph_seal(seed, o, 0x14);
    return 0x14;
}
size_t dlph_pk_startdist(uint8_t *o, const uint8_t seed[4], uint8_t seq, const uint8_t rid[2])
{
    hdr(o, DLPH_T_STARTDIST, 0x14, 1, seq, rid[0], rid[1]);
    o[0x10] = 1;
    dlph_seal(seed, o, 0x14);
    return 0x14;
}
size_t dlph_pk_data(uint8_t *o, const uint8_t seed[4], uint32_t block, uint16_t frag, const uint8_t *src, uint16_t n)
{
    size_t sz = 28u + n;
    hdr(o, DLPH_T_DATA, sz, 1, 0, 0, 0);
    o[0x10] = 0x01; o[0x11] = 0x8A; o[0x12] = 0x4A; o[0x13] = 0xE3;   // 8A 4A E3: constant, same in Azahar's model
    be32(o + 0x14, block); be16(o + 0x18, frag); be16(o + 0x1A, n);
    memcpy(o + 0x1C, src, n);
    dlph_seal(seed, o, sz);
    return sz;
}
size_t dlph_pk_finish_req(uint8_t *o, const uint8_t seed[4], uint8_t seq, const uint8_t rid[2], uint32_t next_ack)
{
    hdr(o, DLPH_T_FINISH, 0x18, 1, seq, rid[0], rid[1]);
    o[0x10] = 1;
    be32(o + 0x14, next_ack);
    dlph_seal(seed, o, 0x18);
    return 0x18;
}
size_t dlph_pk_begin_ping(uint8_t *o, const uint8_t seed[4], uint8_t seq, const uint8_t rid[2])
{
    hdr(o, DLPH_T_BEGIN, 0x20, 1, seq, rid[0], rid[1]);
    o[0x10] = 1; o[0x1D] = 8;
    dlph_seal(seed, o, 0x20);
    return 0x20;
}
size_t dlph_pk_begin_final(uint8_t *o, const uint8_t seed[4], uint8_t seq, const uint8_t rid[2], const uint8_t pass[9])
{
    hdr(o, DLPH_T_BEGIN, 0x20, 1, seq, rid[0], rid[1]);
    o[0x10] = 1;
    memcpy(o + 0x14, pass, 9);
    o[0x1D] = 9;
    dlph_seal(seed, o, 0x20);
    return 0x20;
}
