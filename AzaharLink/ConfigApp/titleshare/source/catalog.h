#pragma once
#include <3ds.h>
#include <stdint.h>

#include "ctx.h"

// Elenco dei titoli installati con nome (letto dall'icona, come fa FBI), raggruppati per gioco:
// il gioco, il suo aggiornamento e i suoi DLC stanno sotto lo stesso nome.

#define CAT_MAX_MEMBERS 48

enum { TS_KIND_APP = 0, TS_KIND_UPDATE = 1, TS_KIND_DLC = 2, TS_KIND_OTHER = 3 };

typedef struct {
    u64 tid;
    char code[16];
    char name[48]; // nome del gioco a cui appartiene (anche per aggiornamenti e DLC)
    uint8_t kind;
} ts_item;

typedef struct {
    char name[48];
    int members[CAT_MAX_MEMBERS]; // indici in items: prima il gioco, poi l'aggiornamento, poi i DLC
    int nMembers;
} ts_game;

typedef struct {
    ts_item* items;
    int nItems;
    ts_game* games;
    int nGames;
    int nNamed;      // titoli di cui si e' letto il nome
    int nTried;      // titoli provati (giochi e aggiornamenti: i DLC non hanno icona)
    Result fsError;  // primo errore del metodo "chiedi al sistema" (0 = nessuno)
    int usedNcch;    // titoli letti con il metodo NCCH
    int cacheHits;   // nomi presi da catalog.cache (titolo e versione invariati)
    int cacheMisses; // nomi riletti dall'icona (titoli nuovi o aggiornati, o prima volta)
} ts_catalog;

// Legge i nomi e raggruppa. `progress` (facoltativa) viene chiamata durante la lettura.
int catalog_build(ts_ctx* c, ts_catalog* cat, void (*progress)(int done, int total));
void catalog_free(ts_catalog* cat);

// Scrive nel registro (aperto) una riga "catalogo:" con i numeri dell'ultima catalog_build: titoli, nomi, dalla cache, riletti, esito della lettura a blocchi delle versioni (AM), se la
// cache e' stata salvata. Il catalogo si costruisce prima che il registro si apra: si chiama all'inizio dell'invio e della ricezione.
void catalog_log_summary(void);

uint8_t catalog_kind(u64 tid);

// Etichetta di un titolo, es. "Kid Icarus: Uprising", "... (aggiornamento)", "... (DLC 02)".
void catalog_label(const ts_item* it, char* out, size_t size);

// Nome in chiaro di un titolo gia' installato (dalla sua icona). 0 se riuscito.
// Prima si chiede al sistema (come FBI); se non e' consentito, si legge l'icona decifrandola con le chiavi dell'utente.
int catalog_read_name(ts_ctx* c, ts_catalog* cat, u64 tid, int language, char* out, size_t size);
