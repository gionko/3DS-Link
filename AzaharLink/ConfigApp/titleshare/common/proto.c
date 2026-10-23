#include "proto.h"

#include <string.h>

static void p16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
}

static void p32(uint8_t* p, uint32_t v) {
    for(int i = 0; i < 4; i++) {
        p[i] = (uint8_t) (v >> (8 * i));
    }
}

static void p64(uint8_t* p, uint64_t v) {
    for(int i = 0; i < 8; i++) {
        p[i] = (uint8_t) (v >> (8 * i));
    }
}

static uint16_t g16(const uint8_t* p) {
    return (uint16_t) (p[0] | (p[1] << 8));
}

static uint32_t g32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint64_t g64(const uint8_t* p) {
    return (uint64_t) g32(p) | ((uint64_t) g32(p + 4) << 32);
}

void ts_manifest_finalize(ts_manifest* m) {
    uint32_t first = 0;
    for(int i = 0; i < m->count; i++) {
        m->t[i].blocks = (uint32_t) ((m->t[i].size + TS_BLOCK - 1) / TS_BLOCK);
        m->t[i].firstBlock = first;
        first += m->t[i].blocks;
    }

    m->totalBlocks = first;
}

int ts_block_info(const ts_manifest* m, uint32_t g, int* title, uint64_t* offset, size_t* len) {
    for(int i = 0; i < m->count; i++) {
        const ts_title* t = &m->t[i];
        if(g >= t->firstBlock && g < t->firstBlock + t->blocks) {
            uint64_t off = (uint64_t) (g - t->firstBlock) * TS_BLOCK;
            uint64_t left = t->size - off;
            *title = i;
            *offset = off;
            *len = left > TS_BLOCK ? TS_BLOCK : (size_t) left;
            return 1;
        }
    }

    return 0;
}

static size_t put_hdr(uint8_t* out, size_t cap, size_t need, uint8_t type, uint8_t flags, uint32_t session, uint16_t token) {
    if(need > cap) {
        return 0;
    }

    p16(out, TS_MAGIC);
    out[2] = type;
    out[3] = flags;
    p32(out + 4, session);
    p16(out + 8, token);
    p16(out + 10, 0);
    return need;
}

#define ENTRY_SIZE 90 // tid 8 + size 8 + hasSeed 1 + seed 16 + code 16 + name 40 + kind 1 (firstBlock non si trasmette: si ricalcola dalle dimensioni). 16 voci + 24 = 1464 byte: il manifesto sta in un frame
#define MANIFEST_HDR 24 // intestazione (12) + count, first, n, riservato (8) + totalBlocks (4)

int ts_manifest_parts(const ts_manifest* m) {
    return (m->count + TS_MANIFEST_PER_FRAME - 1) / TS_MANIFEST_PER_FRAME;
}

size_t ts_pack_manifest(uint8_t* out, size_t cap, const ts_manifest* m, int part) {
    int first = part * TS_MANIFEST_PER_FRAME;
    if(m->count > TS_MAX_TITLES || part < 0 || first >= m->count) {
        return 0;
    }

    int n = m->count - first < TS_MANIFEST_PER_FRAME ? m->count - first : TS_MANIFEST_PER_FRAME;
    size_t need = MANIFEST_HDR + (size_t) n * ENTRY_SIZE;
    if(put_hdr(out, cap, need, TS_T_MANIFEST, 0, m->session, 0) == 0) {
        return 0;
    }

    p16(out + 12, (uint16_t) m->count);
    p16(out + 14, (uint16_t) first);
    p16(out + 16, (uint16_t) n);
    p16(out + 18, 0);
    p32(out + 20, m->totalBlocks);
    uint8_t* e = out + MANIFEST_HDR;
    for(int i = 0; i < n; i++, e += ENTRY_SIZE) {
        const ts_title* t = &m->t[first + i];
        p64(e, t->tid);
        p64(e + 8, t->size);
        e[16] = t->hasSeed;
        memcpy(e + 17, t->seed, 16);
        memcpy(e + 33, t->code, 16);
        memcpy(e + 49, t->name, 39);
        e[88] = 0; // il nome ha 39 caratteri al massimo
        e[89] = t->kind;
    }

    return need;
}

size_t ts_pack_simple(uint8_t* out, size_t cap, uint8_t type, uint8_t flags, uint32_t session, uint16_t token) {
    return put_hdr(out, cap, TS_HDR_SIZE, type, flags, session, token);
}

size_t ts_pack_data(uint8_t* out, size_t cap, uint32_t session, uint32_t block, const uint8_t* payload, size_t len) {
    if(len > TS_BLOCK || put_hdr(out, cap, TS_HDR_SIZE + 4 + len, TS_T_DATA, 0, session, 0) == 0) {
        return 0;
    }

    p32(out + 12, block);
    memcpy(out + 16, payload, len);
    return TS_HDR_SIZE + 4 + len;
}

size_t ts_pack_ack(uint8_t* out, size_t cap, uint32_t session, uint16_t token, uint8_t flags, uint32_t cursor, const ts_range* r, int n) {
    if(n < 0 || n > TS_MAX_RANGES) {
        return 0;
    }

    size_t need = TS_HDR_SIZE + 4 + 2 + (size_t) n * 8;
    if(put_hdr(out, cap, need, TS_T_ACK, flags, session, token) == 0) {
        return 0;
    }

    p32(out + 12, cursor);
    p16(out + 16, (uint16_t) n);
    for(int i = 0; i < n; i++) {
        p32(out + 18 + 8 * i, r[i].start);
        p32(out + 22 + 8 * i, r[i].count);
    }

    return need;
}

size_t ts_pack_bitmap(uint8_t* out, size_t cap, uint8_t type, uint8_t flags, uint32_t session, uint16_t token, int count, const uint8_t* bits) {
    if(count < 0 || count > TS_MAX_TITLES) {
        return 0;
    }

    size_t nb = (size_t) (count + 7) / 8;
    size_t need = TS_HDR_SIZE + 2 + nb;
    if(put_hdr(out, cap, need, type, flags, session, token) == 0) {
        return 0;
    }

    p16(out + 12, (uint16_t) count);
    memcpy(out + 14, bits, nb);
    return need;
}

size_t ts_pack_roster(uint8_t* out, size_t cap, uint32_t session, const ts_roster* r) {
    if(r->n < 0 || r->n > 16) {
        return 0;
    }

    size_t need = TS_HDR_SIZE + 16 + (size_t) r->n * 8 + (r->ext ? 1 + (size_t) r->n * 3 : 0);
    if(put_hdr(out, cap, need, TS_T_ROSTER, 0, session, 0) == 0) {
        return 0;
    }

    p32(out + 12, r->senderNext);
    p32(out + 16, r->rateKiBs);
    p16(out + 20, (uint16_t) r->n);
    p16(out + 22, r->roomEtaSec);
    p32(out + 24, r->senderMs);
    for(int i = 0; i < r->n; i++) {
        p16(out + 28 + 8 * i, r->token[i]);
        p16(out + 30 + 8 * i, r->ageDs[i]);
        p32(out + 32 + 8 * i, r->cursor[i]);
    }

    if(r->ext) {
        uint8_t* t = out + 28 + 8 * r->n;
        t[0] = (uint8_t) r->n;
        for(int i = 0; i < r->n; i++) {
            t[1 + 3 * i] = r->node[i];
            t[2 + 3 * i] = r->state[i];
            t[3 + 3 * i] = r->down[i];
        }
    }

    return need;
}

int ts_unpack_roster(const uint8_t* in, size_t n, ts_roster* r) {
    if(n < TS_HDR_SIZE + 16) {
        return 0;
    }

    int c = g16(in + 20);
    if(c > 16 || n < TS_HDR_SIZE + 16 + (size_t) c * 8) {
        return 0;
    }

    r->senderNext = g32(in + 12);
    r->rateKiBs = g32(in + 16);
    r->senderMs = g32(in + 24);
    r->roomEtaSec = g16(in + 22);
    r->n = c;
    for(int i = 0; i < c; i++) {
        r->token[i] = g16(in + 28 + 8 * i);
        r->ageDs[i] = g16(in + 30 + 8 * i);
        r->cursor[i] = g32(in + 32 + 8 * i);
    }

    r->ext = 0;
    size_t base = TS_HDR_SIZE + 16 + (size_t) c * 8;
    if(n >= base + 1 + (size_t) c * 3 && in[base] == (uint8_t) c) {
        r->ext = 1;
        for(int i = 0; i < c; i++) {
            r->node[i] = in[base + 1 + 3 * (size_t) i];
            r->state[i] = in[base + 2 + 3 * (size_t) i];
            r->down[i] = in[base + 3 + 3 * (size_t) i];
        }
    }

    return 1;
}

uint32_t ts_remaining_blocks(const ts_range* skip, int nSkip, uint32_t from, uint32_t total) {
    if(from >= total) {
        return 0;
    }

    uint64_t left = total - from;
    for(int i = 0; i < nSkip; i++) {
        uint64_t s = skip[i].start, e = s + skip[i].count;
        if(e <= from || s >= total) {
            continue;
        }

        uint64_t a = s > from ? s : from, b = e < total ? e : total;
        left -= b - a;
    }

    return (uint32_t) left;
}

void ts_set_hdr_ident(uint8_t* frame, uint16_t ident) {
    p16(frame + 10, ident);
}

uint16_t ts_hdr_ident(const uint8_t* frame) {
    return g16(frame + 10);
}

uint16_t ts_ident_from(const char* s) {
    if(s == NULL || s[0] == 0) {
        return 0;
    }

    uint32_t h = 2166136261u; // FNV-1a
    for(const char* p = s; *p; p++) {
        h ^= (uint8_t) *p;
        h *= 16777619u;
    }

    uint16_t v = (uint16_t) ((h >> 16) ^ h);
    return v == 0 ? 1 : v;
}

int ts_parse_hdr(const uint8_t* in, size_t n, uint8_t* type, uint8_t* flags, uint32_t* session, uint16_t* token) {
    if(n < TS_HDR_SIZE || g16(in) != TS_MAGIC) {
        return 0;
    }

    *type = in[2];
    *flags = in[3];
    *session = g32(in + 4);
    *token = g16(in + 8);
    return 1;
}

int ts_unpack_manifest(const uint8_t* in, size_t n, ts_manifest* m) {
    if(n < MANIFEST_HDR) {
        return 0;
    }

    int count = g16(in + 12), first = g16(in + 14), cnt = g16(in + 16);
    if(count < 1 || count > TS_MAX_TITLES || first + cnt > count || cnt < 1 || n < MANIFEST_HDR + (size_t) cnt * ENTRY_SIZE) {
        return 0;
    }

    uint32_t session = g32(in + 4);
    if(m->count != 0 && (m->session != session || m->count != count)) {
        return 0; // un'altra sessione: va ignorata
    }

    m->session = session;
    m->count = count;
    m->totalBlocks = g32(in + 20);

    const uint8_t* e = in + MANIFEST_HDR;
    for(int i = 0; i < cnt; i++, e += ENTRY_SIZE) {
        ts_title* t = &m->t[first + i];
        t->tid = g64(e);
        t->size = g64(e + 8);
        if(t->size > 0xFFFFFFFFULL) {
            return 0; // oltre 4 GiB (limite FAT32): un manifesto cosi' non e' valido
        }
        t->hasSeed = e[16];
        memcpy(t->seed, e + 17, 16);
        memcpy(t->code, e + 33, 16);
        t->code[15] = 0;
        memset(t->name, 0, sizeof(t->name));
        memcpy(t->name, e + 49, 39);
        t->kind = e[89];
        t->blocks = (uint32_t) ((t->size + TS_BLOCK - 1) / TS_BLOCK);
        m->have[first + i] = 1;
    }

    if(ts_manifest_complete(m)) {
        uint32_t fb = 0; // il primo blocco di ogni titolo si ricava dalle dimensioni (non si trasmette)
        for(int i = 0; i < m->count; i++) {
            m->t[i].firstBlock = fb;
            fb += m->t[i].blocks;
        }
    }

    return 1;
}

int ts_manifest_complete(const ts_manifest* m) {
    if(m->count < 1) {
        return 0;
    }

    for(int i = 0; i < m->count; i++) {
        if(!m->have[i]) {
            return 0;
        }
    }

    return 1;
}

int ts_unpack_bitmap(const uint8_t* in, size_t n, int* count, uint8_t* bits) {
    if(n < TS_HDR_SIZE + 2) {
        return 0;
    }

    int c = g16(in + 12);
    size_t nb = (size_t) (c + 7) / 8;
    if(c > TS_MAX_TITLES || n < TS_HDR_SIZE + 2 + nb) {
        return 0;
    }

    memset(bits, 0, TS_BITMAP_BYTES);
    memcpy(bits, in + 14, nb);
    if(c & 7) {
        bits[nb - 1] &= (uint8_t) ((1u << (c & 7)) - 1); // i bit oltre `count` non esistono
    }

    *count = c;
    return 1;
}

int ts_unpack_data(const uint8_t* in, size_t n, uint32_t* block, const uint8_t** payload, size_t* len) {
    if(n < TS_HDR_SIZE + 4 || n > TS_HDR_SIZE + 4 + TS_BLOCK) {
        return 0;
    }

    *block = g32(in + 12);
    *payload = in + 16;
    *len = n - 16;
    return 1;
}

int ts_unpack_ack(const uint8_t* in, size_t n, uint32_t* cursor, ts_range* r, int maxRanges, int* nRanges) {
    if(n < TS_HDR_SIZE + 6) {
        return 0;
    }

    int cnt = g16(in + 16);
    if(cnt > TS_MAX_RANGES || n < TS_HDR_SIZE + 6 + (size_t) cnt * 8) {
        return 0;
    }

    *cursor = g32(in + 12);
    if(cnt > maxRanges) {
        cnt = maxRanges;
    }

    for(int i = 0; i < cnt; i++) {
        r[i].start = g32(in + 18 + 8 * i);
        r[i].count = g32(in + 22 + 8 * i);
    }

    *nRanges = cnt;
    return 1;
}

int ts_skip_ranges(const ts_manifest* m, const uint8_t* bits, ts_range* out, int max) {
    int n = 0;
    for(int i = 0; i < m->count; i++) {
        if(ts_bit(bits, i) || m->t[i].blocks == 0) {
            continue;
        }

        if(n > 0 && out[n - 1].start + out[n - 1].count == m->t[i].firstBlock) {
            out[n - 1].count += m->t[i].blocks;
        } else if(n < max) {
            out[n].start = m->t[i].firstBlock;
            out[n].count = m->t[i].blocks;
            n++;
        }
    }

    return n;
}

size_t ts_pack_ack2(uint8_t* out, size_t cap, uint32_t session, uint16_t token, uint8_t flags, const ts_ack2* a) {
    if(a->nSum < 0 || a->nSum > TS_ACK2_MAX_SUM || a->nRanges < 0 || a->nRanges > TS_ACK2_MAX_RANGES) {
        return 0;
    }

    size_t need = TS_HDR_SIZE + 12 + (size_t) a->nSum * 14 + (size_t) a->nRanges * 8;
    if(put_hdr(out, cap, need, TS_T_ACK2, flags, session, token) == 0) {
        return 0;
    }

    uint8_t* p = out + TS_HDR_SIZE;
    p[0] = a->focus;
    p[1] = (uint8_t) a->nSum;
    p16(p + 2, (uint16_t) a->nRanges);
    p32(p + 4, a->freeMiB);
    p32(p + 8, a->installed);
    p += 12;
    for(int i = 0; i < a->nSum; i++, p += 14) {
        p[0] = a->sum[i].title;
        p[1] = a->sum[i].flags;
        p32(p + 2, a->sum[i].remain);
        p32(p + 6, a->sum[i].first);
        p32(p + 10, a->sum[i].inst);
    }

    for(int i = 0; i < a->nRanges; i++, p += 8) {
        p32(p, a->r[i].start);
        p32(p + 4, a->r[i].count);
    }

    return need;
}

int ts_unpack_ack2(const uint8_t* in, size_t n, ts_ack2* a) {
    if(n < TS_HDR_SIZE + 12) {
        return 0;
    }

    const uint8_t* p = in + TS_HDR_SIZE;
    int ns = p[1], nr = g16(p + 2);
    if(ns > TS_ACK2_MAX_SUM || nr > TS_ACK2_MAX_RANGES || n < TS_HDR_SIZE + 12 + (size_t) ns * 14 + (size_t) nr * 8) {
        return 0;
    }

    a->focus = p[0];
    a->freeMiB = g32(p + 4);
    a->installed = g32(p + 8);
    a->nSum = ns;
    a->nRanges = nr;
    p += 12;
    for(int i = 0; i < ns; i++, p += 14) {
        a->sum[i].title = p[0];
        a->sum[i].flags = p[1];
        a->sum[i].remain = g32(p + 2);
        a->sum[i].first = g32(p + 6);
        a->sum[i].inst = g32(p + 10);
    }

    for(int i = 0; i < nr; i++, p += 8) {
        a->r[i].start = g32(p);
        a->r[i].count = g32(p + 4);
    }

    return 1;
}

size_t ts_pack_state(uint8_t* out, size_t cap, uint32_t session, uint8_t title, uint16_t gen, uint8_t nRecv) {
    size_t need = TS_HDR_SIZE + 4;
    if(put_hdr(out, cap, need, TS_T_STATE, 0, session, 0) == 0) {
        return 0;
    }

    out[12] = title;
    out[13] = nRecv;
    p16(out + 14, gen);
    return need;
}

int ts_unpack_state(const uint8_t* in, size_t n, uint8_t* title, uint16_t* gen, uint8_t* nRecv) {
    if(n < TS_HDR_SIZE + 4) {
        return 0;
    }

    *title = in[12];
    *nRecv = in[13];
    *gen = g16(in + 14);
    return 1;
}

size_t ts_pack_ready(uint8_t* out, size_t cap, uint8_t flags, uint32_t session, uint16_t token, int count, const uint8_t* bits, uint32_t freeMiB) {
    size_t n = ts_pack_bitmap(out, cap, TS_T_READY, flags, session, token, count, bits);
    if(n == 0 || n + 4 > cap) {
        return 0;
    }

    p32(out + n, freeMiB);
    return n + 4;
}

uint32_t ts_unpack_free_mib(const uint8_t* in, size_t n) {
    if(n < TS_HDR_SIZE + 2) {
        return 0xFFFFFFFFu;
    }

    size_t nb = (size_t) (g16(in + 12) + 7) / 8;
    if(g16(in + 12) > TS_MAX_TITLES || n < TS_HDR_SIZE + 2 + nb + 4) {
        return 0xFFFFFFFFu;
    }

    return g32(in + TS_HDR_SIZE + 2 + nb);
}
