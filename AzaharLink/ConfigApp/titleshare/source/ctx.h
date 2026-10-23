#pragma once
#include <3ds.h>
#include <stddef.h>
#include <stdint.h>

// Stato condiviso del programma: chiavi, percorso della SD, database dei certificati e dei seed, elenco dei titoli.

typedef struct {
    uint32_t rk[44]; // chiave AES dello strato SD, espansa
    char id0[33];
    char devId[160]; // percorso /Nintendo 3DS/<ID0>/<ID1> della SD (anche senza chiavi): da qui si ricava l'identificativo stabile della console
    char ctrRoot[512]; // "sdmc:/Nintendo 3DS/<ID0>/<ID1>"
    uint8_t* certsDb;
    size_t certsDbSize;
    uint8_t* seedDb;
    size_t seedDbSize;
    u64* titleIds;
    u32 titleCount;
    bool isNew;
    bool haveKeys; // false = solo ricezione (nessun boot9.bin / movable.sed)
    uint8_t keyX2C[16]; // KeyX slot 0x2C (strato NCCH: serve a leggere i nomi dalle icone)
    bool haveNcchKey;
} ts_ctx;

// Inizializza tutto. Se mancano le chiavi, haveKeys resta false e il programma puo' solo ricevere.
// Ritorna 0 se riuscito (anche senza chiavi); un numero negativo solo se non si puo' proseguire.
int ctx_init(ts_ctx* c, char* msg, size_t msgSize);
void ctx_exit(ts_ctx* c);

void ctx_product_code(u64 tid, char out[16]);
