#pragma once
#include <stdint.h>

// Cache dei nomi dei titoli installati (catalog.cache): leggere il nome di ogni titolo (icona, a volte decifratura NCCH) a ogni avvio e' lento; si rilegge
// solo cio' che e' nuovo o ha cambiato versione. Codice puro (solo stdio), provato su PC (catcache_test).

typedef struct {
    uint64_t tid;
    uint16_t ver;     // versione del titolo quando il nome e' stato letto: se cambia, si rilegge
    char name[48];
} ts_ccent;

typedef struct {
    ts_ccent* e;
    int n, cap;
    int lang;   // lingua di sistema: i nomi dipendono da lei; una cache di un'altra lingua non vale
} ts_catcache;

void catcache_init(ts_catcache* c, int lang);
void catcache_free(ts_catcache* c);
// Carica il file. Ritorna il numero di voci caricate (0 se manca, e' di un'altra lingua/versione del formato o e' corrotto: il chiamante parte da zero).
int catcache_load(ts_catcache* c, const char* path);
// Il nome del titolo se c'e' con quella versione, altrimenti NULL.
const char* catcache_get(const ts_catcache* c, uint64_t tid, uint16_t ver);
// Aggiunge (o sostituisce) una voce. Ritorna 0 se riuscito.
int catcache_put(ts_catcache* c, uint64_t tid, uint16_t ver, const char* name);
// Scrive il file (prima un file temporaneo, poi lo sostituisce). Ritorna 0 se riuscito.
int catcache_save(const ts_catcache* c, const char* path);
