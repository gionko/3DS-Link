#pragma once
#include <stddef.h>
#include <stdint.h>

#include "proto.h"

// Cache delle ripetizioni del mittente: le ultime corse di blocchi consecutivi lette dal CIA virtuale (SD + AES software, ~13-14 ms per
// 16 blocchi). Una ripetizione fuori dall'anello del lettore cerca il blocco in TUTTE le corse; se non c'e' legge una corsa nuova (da quel
// blocco fino a runBlocks blocchi, senza uscire dal titolo) al posto della meno usata di recente (LRU). In recupero i flussi di blocchi
// consecutivi sono 3-4 o piu' (marcia, ritardatari, buchi del turno): con due sole corse a turno si rilegge quasi sempre, con 8-16 ogni
// corsa serve per intero (rapporto sul mittente, simulazione di difficolta-4).
//
// Codice puro (nessuna dipendenza dalla console): la lettura e' una funzione del chiamante (sul 3DS cia_stream_read dopo open_title, sui
// PC di prova un file finto), la memoria e' un blocco unico allocato dal chiamante. Sta in UN SOLO thread (quello di invio): niente
// blocchi, niente atomici. Provato su PC (share/tests/runcache_test.c), anche sotto ASan+UBSan. Il percorso caldo non alloca e non divide.

// Ricerca lineare in tutte le corse (nessun indice): ~9 istruzioni ARM per corsa, cioe' ~0,7k cicli a 64 corse (2,6 us a 268 MHz) nel caso peggiore, e
// ci si arriva solo dopo che l'anello non aveva il blocco (~400 volte al secondo in recupero): trascurabile accanto ai ~13 ms di una lettura dalla SD.
#define TS_RC_MAX_SLOTS 64        // corse al massimo

#define TS_RC_MAX_RUN_BLOCKS 4096 // blocchi al massimo per corsa

// Una lettura chiesta alla funzione del chiamante: `count` blocchi consecutivi a partire dal blocco globale `g`, tutti dello stesso titolo.
typedef struct {
    uint32_t g;     // primo blocco globale
    uint32_t count; // blocchi (1..runBlocks)
    int title;      // titolo del manifesto
    uint64_t off;   // offset del primo blocco nel CIA del titolo
    size_t bytes;   // byte da leggere in dst (count * TS_BLOCK, meno nell'ultimo blocco del titolo, che e' corto)
} ts_rc_req;

// Esiti della funzione di lettura.
enum {
    TS_RC_OK = 0,    // dst contiene i `bytes` richiesti
    TS_RC_ERR = -1,  // lettura fallita: dst puo' contenere dati parziali (la corsa che lo ospitava si scarta)
    TS_RC_NOOP = 1,  // niente letto e niente scritto in dst (non tentata o fallita prima di leggere): la corsa che lo ospita resta com'era
};

typedef int (*ts_rc_read_fn)(void* ctx, const ts_rc_req* rq, uint8_t* dst);

// Esiti di ts_rc_get / ts_rc_fill (campo `how` di ts_rc_info).
enum {
    TS_RC_HIT = 0,     // il blocco era in una corsa
    TS_RC_FILLED = 1,  // letto da una corsa nuova
    TS_RC_FAILED = -1, // lettura non riuscita (la funzione di lettura ha risposto TS_RC_ERR o TS_RC_NOOP)
    TS_RC_RANGE = -2,  // blocco fuori dal manifesto
};

typedef struct {
    int how;
    int title;      // titolo del blocco (valido solo se how == TS_RC_FILLED o TS_RC_FAILED; con TS_RC_HIT e TS_RC_RANGE vale -1)
    uint32_t count; // blocchi della corsa letta (valido solo se how == TS_RC_FILLED o TS_RC_FAILED; altrimenti 0)
    uint32_t slot;  // corsa usata (HIT) o scelta (FILLED, FAILED); 0 con TS_RC_RANGE
} ts_rc_info;

typedef struct {
    uint32_t start; // primo blocco globale della corsa
    uint32_t count; // blocchi validi (0 = corsa vuota)
    uint32_t use;   // marca dell'ultimo uso (contatore monotono, vedi tick)
} ts_rc_slot;

typedef struct {
    const ts_manifest* m;
    uint8_t* mem;       // nSlots * runBlocks * TS_BLOCK byte, del chiamante
    ts_rc_read_fn read;
    void* ctx;
    uint32_t nSlots, runBlocks;
    // LRU: a ogni uso (hit o corsa nuova) tick cresce di 1 e la corsa usata prende quel valore. L'eta' di una corsa e' tick - use in aritmetica
    // senza segno: esatta anche dopo il giro a 2^32 finche' nessuna corsa resta ferma per piu' di 2^32 usi (a 1450 usi/s, il massimo: ~34 giorni di
    // invio ininterrotto; il contatore riparte da zero a ogni sessione, quindi nella pratica il giro non c'e' mai: e' provato lo stesso).
    uint32_t tick;
    uint32_t hint; // ultima corsa usata: i flussi sequenziali ritrovano il blocco subito
    ts_rc_slot s[TS_RC_MAX_SLOTS];
    // statistiche
    uint64_t hits, fills, failed, noops, range, blocksRead;
} ts_runcache;

// Byte di memoria che servono.
size_t ts_rc_mem_size(uint32_t nSlots, uint32_t runBlocks);

// Prepara la cache (tutte le corse vuote). 0 se riuscito, -1 se i parametri non valgono (nSlots 1..TS_RC_MAX_SLOTS, runBlocks
// 1..TS_RC_MAX_RUN_BLOCKS, mem grande ts_rc_mem_size). `m` deve restare valido e invariato finche' la cache si usa.
int ts_rc_init(ts_runcache* c, const ts_manifest* m, uint8_t* mem, uint32_t nSlots, uint32_t runBlocks, ts_rc_read_fn read, void* ctx);

// Svuota tutte le corse / quelle del titolo (indice nel manifesto). Le statistiche restano.
void ts_rc_invalidate_all(ts_runcache* c);
void ts_rc_invalidate_title(ts_runcache* c, int title);

// Il blocco g e' in una corsa? Ritorna i suoi byte (validi fino alla prossima ts_rc_fill) o NULL. Il blocco conta come usato.
const uint8_t* ts_rc_find(ts_runcache* c, uint32_t g);

// Legge da una corsa nuova (la meno usata di recente, o una vuota) la corsa che comincia in g e ritorna i byte di g, o NULL se la
// lettura non e' riuscita o g e' fuori dal manifesto (info->how lo dice; info puo' essere NULL). Una corsa mai diventa valida con dati
// parziali: durante la lettura e' vuota e lo resta se la lettura non riesce (tranne TS_RC_NOOP, che la ripristina).
// Non cerca prima in cache: e' il passo dopo ts_rc_find.
const uint8_t* ts_rc_fill(ts_runcache* c, uint32_t g, ts_rc_info* info);

// ts_rc_find e, se manca, ts_rc_fill.
const uint8_t* ts_rc_get(ts_runcache* c, uint32_t g, ts_rc_info* info);

// Per le prove: la corsa dello slot i. 1 se e' valida (count > 0); i puntatori di uscita possono essere NULL.
int ts_rc_slot_info(const ts_runcache* c, uint32_t i, uint32_t* start, uint32_t* count, uint32_t* use, const uint8_t** data);
