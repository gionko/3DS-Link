#pragma once
#include <stdint.h>
#include <stdio.h>

#include "cia_front.h"
#include "contentpath.h"

// CIA virtuale di un titolo installato sulla SD: i byte del CIA standard si ricalcolano a richiesta
// (parte fissa in RAM, content letti dai .app e decifrati dallo strato SD). Nessun file temporaneo.

typedef struct {
    cia_front front;
    char contentDir[512]; // percorso reale
    char contentRel[512]; // percorso relativo a ID1 (serve al contatore AES)
    uint8_t ctr[CIA_MAX_CONTENTS][16];
    content_dirs dirs;
    int8_t sub[CIA_MAX_CONTENTS]; // sottocartella di content/ di ogni content (-1 = nessuna)
    const uint32_t* rk;
    FILE* file;
    int fileIdx;
    char err[160];
    // Statistiche di lettura (solo sulla console): tick passati in fseek+fread e nella decifratura, byte letti dai content.
    uint64_t tkFread, tkAes, bytesRead;
} cia_stream;

// Legge il file certs.db dal NAND (sola lettura). Ritorna 0 se riuscito; *out va liberato con free().
int cia_load_certs_db(uint8_t** out, size_t* size);

// ctrRoot: "sdmc:/Nintendo 3DS/<ID0>/<ID1>"; rk: chiave AES gia' espansa dello strato SD.
int cia_stream_open(cia_stream* s, uint64_t tid, const char* ctrRoot, const uint32_t rk[44], const uint8_t* certsDb, size_t certsDbSize);

// Legge `len` byte del CIA a partire da `off`. 0 se riuscito.
int cia_stream_read(cia_stream* s, uint64_t off, uint8_t* buf, size_t len);

void cia_stream_close(cia_stream* s);
