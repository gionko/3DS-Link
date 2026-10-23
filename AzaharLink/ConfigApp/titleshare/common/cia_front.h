#pragma once
#include <stddef.h>
#include <stdint.h>

// Parte "fissa" di un CIA standard (header, catena di certificati, ticket generico, TMD) costruita a partire
// dal TMD di un titolo installato. Codice puro, senza dipendenze dalla console, quindi provabile sul PC.
//
// Il CIA prodotto ha i content gia' in chiaro (flag di cifratura azzerato nel TMD, hash del TMD ricalcolati)
// e nessuna sezione meta. La firma originale del TMD non e' piu' valida: serve la patch delle firme (Luma3DS).

#define CIA_MAX_CONTENTS 4096
#define CIA_CERT_SIZE 0xA00
#define CIA_FRONT_MAX 0x34000

typedef struct {
    uint32_t id;
    uint16_t index;
    uint16_t type;
    uint64_t size;
} cia_content;

typedef struct {
    uint64_t tid;
    int n;
    cia_content c[CIA_MAX_CONTENTS];
    uint64_t start[CIA_MAX_CONTENTS]; // offset di ogni content nel CIA
    uint64_t contentStart;            // = frontSize
    uint64_t total;                   // dimensione del CIA (senza meta)
    size_t frontSize;
    size_t offCert, offTicket, offTmd;
    uint8_t front[CIA_FRONT_MAX];
} cia_front;

// Ritorna 0 se riuscito. `tmd` e' il TMD in chiaro; `certs` e' la catena CA+XS+CP (0xA00 byte).
int cia_front_build(cia_front* f, const uint8_t* tmd, size_t tmdSize, const uint8_t certs[CIA_CERT_SIZE]);

// Nome del certificato CP richiesto dal TMD (es. "CP0000000b"); out ha almeno 64 byte. 0 se riuscito.
int cia_tmd_cp_name(const uint8_t* tmd, size_t tmdSize, char* out);

// Cerca in un certs.db (o qualsiasi blob) i certificati e compone la catena CA + XS0000000c + CP come GodMode9.
// 0 se riuscito, altrimenti un numero negativo: -1 CA mancante, -2 XS mancante, -3 CP mancante, -4 dimensioni inattese.
int cia_find_certs(const uint8_t* db, size_t size, const char* cpName, uint8_t out[CIA_CERT_SIZE]);
