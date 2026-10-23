#include "cia_front.h"

#include <string.h>

#include "sha256.h"
#include "ticket_template.h"

#define ALIGN64(x) (((x) + 0x3Full) & ~0x3Full)

static uint16_t be16(const uint8_t* p) {
    return (uint16_t) ((p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t* p) {
    return ((uint64_t) be32(p) << 32) | be32(p + 4);
}

static void put_be16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t) (v >> 8);
    p[1] = (uint8_t) v;
}

static void put_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
}

static void put_le32(uint8_t* p, uint32_t v) {
    for(int i = 0; i < 4; i++) {
        p[i] = (uint8_t) (v >> (8 * i));
    }
}

static void put_le64(uint8_t* p, uint64_t v) {
    for(int i = 0; i < 8; i++) {
        p[i] = (uint8_t) (v >> (8 * i));
    }
}

// Dimensione della firma (con il padding) di un blocco firmato, 0 se il tipo e' sconosciuto.
static size_t sig_block_size(uint32_t sigType) {
    switch(sigType) {
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

int cia_tmd_cp_name(const uint8_t* tmd, size_t tmdSize, char* out) {
    if(tmdSize < 8) {
        return -1;
    }

    size_t sig = sig_block_size(be32(tmd));
    if(sig == 0 || sig + 0x40 > tmdSize) {
        return -2;
    }

    char issuer[0x41];
    memcpy(issuer, tmd + sig, 0x40);
    issuer[0x40] = 0;

    const char* dash = strrchr(issuer, '-');
    if(dash == NULL || strlen(dash + 1) >= 64) {
        return -3;
    }

    strcpy(out, dash + 1);
    return 0;
}

int cia_front_build(cia_front* f, const uint8_t* tmdIn, size_t tmdSize, const uint8_t certs[CIA_CERT_SIZE]) {
    memset(f, 0, sizeof(*f));

    size_t sig = sig_block_size(be32(tmdIn));
    if(sig == 0) {
        return -1;
    }

    size_t hb = ALIGN64(sig);
    if(hb + 0xC4 + 0x24 * 64 > tmdSize) {
        return -2;
    }

    int n = be16(tmdIn + hb + 0x9E);
    size_t cr = hb + 0xC4 + 0x24 * 64;
    size_t tmdLen = cr + 0x30 * (size_t) n;
    if(n < 1 || n > CIA_MAX_CONTENTS || tmdLen > tmdSize) {
        return -3;
    }

    f->tid = be64(tmdIn + hb + 0x4C);
    f->n = n;

    // Sezioni
    f->offCert = 0x2040;
    f->offTicket = f->offCert + ALIGN64(CIA_CERT_SIZE);
    f->offTmd = f->offTicket + ALIGN64(TICKET_TEMPLATE_SIZE);
    size_t front = f->offTmd + ALIGN64(tmdLen);
    if(front > CIA_FRONT_MAX) {
        return -4;
    }

    f->frontSize = front;
    f->contentStart = front;

    // Certificati
    memcpy(f->front + f->offCert, certs, CIA_CERT_SIZE);

    // Ticket generico con il title ID
    memcpy(f->front + f->offTicket, TICKET_TEMPLATE, TICKET_TEMPLATE_SIZE);
    for(int i = 0; i < 8; i++) {
        f->front[f->offTicket + TICKET_TID_OFFSET + i] = (uint8_t) (f->tid >> (56 - 8 * i));
    }

    // TMD: content in chiaro (flag di cifratura azzerato) e catena di hash ricalcolata.
    uint8_t* tmd = f->front + f->offTmd;
    memcpy(tmd, tmdIn, tmdLen);

    uint64_t contentSize = 0;
    for(int i = 0; i < n; i++) {
        uint8_t* r = tmd + cr + 0x30 * (size_t) i;
        uint16_t type = be16(r + 6);
        type = (uint16_t) (type & ~0x0001);
        put_be16(r + 6, type);

        f->c[i].id = be32(r);
        f->c[i].index = be16(r + 4);
        f->c[i].type = type;
        f->c[i].size = be64(r + 8);

        f->start[i] = f->contentStart + contentSize;
        contentSize += ALIGN64(f->c[i].size);
    }

    f->total = f->contentStart + contentSize;

    // Catena di hash: ogni info record copre un gruppo di chunk (indice, numero) e ne porta lo SHA-256;
    // l'header porta lo SHA-256 di tutti i 64 info record. Si mantiene il raggruppamento originale.
    uint8_t* info = tmd + hb + 0xC4;
    for(int k = 0; k < 64; k++) {
        uint8_t* r = info + 0x24 * (size_t) k;
        size_t first = be16(r), count = be16(r + 2);
        if(count == 0) {
            continue;
        }

        if(first + count > (size_t) n) {
            return -5;
        }

        sha256(tmd + cr + 0x30 * first, 0x30 * count, r + 4);
    }

    sha256(info, 0x24 * 64, tmd + hb + 0xA4);

    // Header del CIA
    uint8_t* h = f->front;
    put_le32(h + 0x00, 0x2020);
    put_le16(h + 0x04, 0); // tipo
    put_le16(h + 0x06, 0); // versione
    put_le32(h + 0x08, CIA_CERT_SIZE);
    put_le32(h + 0x0C, TICKET_TEMPLATE_SIZE);
    put_le32(h + 0x10, (uint32_t) tmdLen);
    put_le32(h + 0x14, 0); // nessuna sezione meta
    put_le64(h + 0x18, contentSize);
    for(int i = 0; i < n; i++) {
        uint16_t idx = f->c[i].index;
        h[0x20 + idx / 8] |= (uint8_t) (0x80 >> (idx % 8));
    }

    return 0;
}

// ---------------------------------------------------------------- ricerca dei certificati

typedef struct {
    size_t size;
    char issuer[0x41];
    char name[0x41];
} cert_info;

// Valida un certificato che comincia a db + pos.
static int parse_cert(const uint8_t* db, size_t size, size_t pos, cert_info* ci) {
    if(pos + 4 > size) {
        return 0;
    }

    size_t sig = sig_block_size(be32(db + pos));
    if(sig == 0 || pos + sig + 0x88 > size) {
        return 0;
    }

    const uint8_t* issuer = db + pos + sig;
    uint32_t keyType = be32(issuer + 0x40);
    const uint8_t* name = issuer + 0x44;

    size_t keySize;
    if(keyType == 0) {
        keySize = 0x200 + 4 + 0x34;
    } else if(keyType == 1) {
        keySize = 0x100 + 4 + 0x34;
    } else if(keyType == 2) {
        keySize = 0x3C + 0x3C;
    } else {
        return 0;
    }

    size_t total = sig + 0x40 + 4 + 0x40 + 4 + keySize;
    if(pos + total > size) {
        return 0;
    }

    for(int i = 0; i < 0x40; i++) {
        uint8_t a = issuer[i], b = name[i];
        if((a != 0 && (a < 0x20 || a > 0x7E)) || (b != 0 && (b < 0x20 || b > 0x7E))) {
            return 0;
        }
    }

    memcpy(ci->issuer, issuer, 0x40);
    ci->issuer[0x40] = 0;
    memcpy(ci->name, name, 0x40);
    ci->name[0x40] = 0;
    ci->size = total;
    return 1;
}

static int find_cert(const uint8_t* db, size_t size, const char* issuer, const char* name, const uint8_t** out, size_t* outSize) {
    for(size_t pos = 0; pos + 4 <= size; pos += 4) {
        cert_info ci;
        if(parse_cert(db, size, pos, &ci) && strcmp(ci.issuer, issuer) == 0 && strcmp(ci.name, name) == 0) {
            *out = db + pos;
            *outSize = ci.size;
            return 1;
        }
    }

    return 0;
}

int cia_find_certs(const uint8_t* db, size_t size, const char* cpName, uint8_t out[CIA_CERT_SIZE]) {
    const uint8_t* ca3;
    const uint8_t* ca4;
    const uint8_t* xs;
    const uint8_t* cp;
    size_t ca3Size, ca4Size, xsSize, cpSize;

    if(!find_cert(db, size, "Root", "CA00000003", &ca3, &ca3Size) || !find_cert(db, size, "Root", "CA00000004", &ca4, &ca4Size)) {
        return -1;
    }

    if(!find_cert(db, size, "Root-CA00000003", "XS0000000c", &xs, &xsSize)) {
        return -2;
    }

    if(!find_cert(db, size, "Root-CA00000003", cpName, &cp, &cpSize)) {
        return -3;
    }

    if(ca3Size != 0x400 || ca4Size != 0x400 || xsSize != 0x300 || cpSize != 0x300) {
        return -4;
    }

    // Come GodMode9: il certificato CA00000003 dei CIA ha i primi 0x1F0 byte (tipo e firma) presi da CA00000004.
    memcpy(out, ca4, 0x1F0);
    memcpy(out + 0x1F0, ca3 + 0x1F0, 0x400 - 0x1F0);
    memcpy(out + 0x400, xs, xsSize);
    memcpy(out + 0x400 + xsSize, cp, cpSize);
    return 0;
}
