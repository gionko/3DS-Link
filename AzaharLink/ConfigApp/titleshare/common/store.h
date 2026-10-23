#pragma once
#include <stddef.h>
#include <stdint.h>

#include "proto.h"
#include "win.h"

// Archivio del ricevente: tiene cio' che passa per ogni titolo voluto e lo rende disponibile all'installazione, in ordine, indipendentemente
// da come arriva dalla rete. Vedi PROGETTO-CAROSELLO.md (§2.2, §2.3).
//
//   - Un solo titolo e' "attivo" (quello che AM sta installando): i suoi blocchi dentro la finestra in RAM (ts_win) vanno li'.
//   - Tutti gli altri blocchi voluti (titoli non attivi, o blocchi del titolo attivo oltre la finestra) vanno in una coda di parcheggio in RAM
//     e da li' in un file temporaneo per titolo (store_park_step, thread dedicato), sempre scrivendo in coda al file.
//   - Politica di scrittura del parcheggio ("dal piu' vecchio, con ritardo"): ogni voce della coda porta l'istante d'arrivo. Il parcheggio guarda la
//     voce piu' VECCHIA viva (la testa) e, solo quando ha almeno ST_AGE_MS (circa 3 s, meno se la coda e' corta), scrive il tratto contiguo
//     per indice che la CONTIENE, esteso a sinistra e a destra con cio' che c'e' in coda (al massimo ST_MAX_RUN blocchi). Nell'attesa i buchi si
//     riparano e i vicini arrivano, quindi sul file i blocchi stanno in ordine di indice e l'installazione legge poche volte, lunghe. Si scrive
//     subito, senza aspettare l'eta', solo se la coda e' al 75% (valvola: non si scarta) o il mittente non ha piu' niente da inviare (store_set_idle).
//   - L'installatore non aspetta mai il parcheggio: un blocco dichiarato disponibile ma ancora in coda (parcheggiato oltre la finestra mentre era a
//     secco, o di un titolo appena attivato) lo prende direttamente da li' (store_next_run, ST_SRC_QUEUE): niente scrittura e rilettura sulla SD.
//   - Disponibile = in finestra oppure presente sul file. L'installazione legge dalla finestra e, dove manca, dal file.
//
// Logica pura, senza dipendenze dalla console: il file passa da un'interfaccia (ts_fileio), in memoria nelle prove su PC.
// Thread (console): rete -> store_offer (e store_set_idle); parcheggio -> store_park_step (e store_update_space); installazione -> store_pick_active,
// store_activate, store_next_run, store_finish; riscontri -> store_summary, store_missing. Le strutture sono protette da un lock interno, mai tenuto
// durante I/O.

typedef struct {
    void* ctx;
    // Prepara il file temporaneo del titolo (0 = ok). Chiamata prima della prima scrittura.
    int (*open)(void* ctx, int title);
    // Scrive in coda; *off = posizione del primo byte scritto. 0 = ok, negativo = errore (spazio finito, SD tolta).
    int (*append)(void* ctx, int title, const uint8_t* data, size_t len, uint64_t* off);
    // Legge len byte a partire da off. 0 = ok.
    int (*read)(void* ctx, int title, uint64_t off, uint8_t* out, size_t len);
    // Chiude e cancella il file del titolo.
    void (*remove)(void* ctx, int title);
    // Orologio in millisecondi a 32 bit (puo' girare: si confrontano solo differenze). Serve all'eta' delle voci della coda di parcheggio.
    // NULL = nessun orologio: l'eta' non conta e il parcheggio scrive la testa subito.
    uint32_t (*now_ms)(void* ctx);
} ts_fileio;

enum { ST_PENDING = 0, ST_INSTALLING = 1, ST_DONE = 2, ST_ERROR = 3 };
enum { ST_OFFER_IGNORE = -1, ST_OFFER_DROP = -2, ST_OFFER_DUP = 0, ST_OFFER_WIN = 1, ST_OFFER_PARK = 2 };
enum { ST_F_ACCEPT = 1, ST_F_INSTALLING = 2, ST_F_DONE = 4, ST_F_ERROR = 8, ST_F_PARKOK = 16 };
enum { ST_SRC_NONE = 0, ST_SRC_WIN = 1, ST_SRC_FILE = 2, ST_SRC_QUEUE = 4 }; // bit diversi: le prove li accumulano

// ts_sum (voce di riepilogo) e' definito in proto.h; i flag ST_F_* coincidono con TS_SF_*.

// Politica di scrittura del parcheggio (vedi sopra e store_park_step).
#define ST_MAX_RUN 64       // blocchi per scrittura sul file (e per lettura dell'installatore: un tratto)
#define ST_AGE_MS 3000      // eta' minima della voce piu' vecchia della coda prima che il parcheggio la scriva ...
#define ST_AGE_FILL_PCT 40  // ... ridotta, per le code corte, al tempo che al ritmo d'arrivo attuale serve a riempirne il 40%
#define ST_RATE_MS 500      // finestra minima di stima del ritmo d'arrivo dei blocchi in coda
#define ST_IDLE_MS 1000     // il mittente senza niente da inviare da tanto tempo: fine mandata, si scrive subito

// Oltre la finestra del titolo attivo (PARKOK nel riepilogo e accettazione in store_offer) si parcheggia fino a coda piena (0, di base: soglia spenta) o solo con la coda del
// parcheggio sotto il 75% (1). Si cambia con -DTS_PARKOK_SOTTO_75=1 / make PARKOK75=1; vedi store.c e INTEG1-NOTE §5.4.
#ifndef TS_PARKOK_SOTTO_75
#define TS_PARKOK_SOTTO_75 0 // di base SPENTO (14.10): a 4+ riceventi con parcheggio lento ritarda i primi riceventi (+45%...+192% nel modello); make PARKOK75=1 per accenderlo
#endif

typedef struct ts_store ts_store;

// want = bitmap dei titoli voluti (come in READY). win = finestra gia' allocata (non si possiede). parkSlots = capacita' della coda di
// parcheggio in blocchi. Ritorna NULL se manca memoria.
ts_store* store_create(const ts_manifest* m, const uint8_t* want, const ts_fileio* io, ts_win* win, uint32_t parkSlots);
void store_destroy(ts_store* s);

// Un blocco arrivato dalla rete. Ritorna uno degli ST_OFFER_*.
int store_offer(ts_store* s, uint32_t g, const uint8_t* payload, size_t len);

// Scrive sul file al massimo una "corsa" di blocchi contigui (per indice) della coda di parcheggio: quella che contiene la voce piu' vecchia, se ha
// l'eta' richiesta (o se vale una delle eccezioni elencate in cima), altrimenti nessuna. Ritorna 1 se ha fatto qualcosa (scritto o scartato),
// 0 se non c'e' niente da fare ora (coda vuota, oppure la voce piu' vecchia e' ancora troppo giovane: si richiama fra pochi ms), -1 se la
// scrittura e' fallita (il titolo smette di accettare; *title dice quale).
int store_park_step(ts_store* s, int* title);
// Blocchi in coda di parcheggio (per decidere se dormire).
uint32_t store_park_queued(ts_store* s);

// Quale titolo attivare ora (-1 = nessuno). `curHint` = titolo che il mittente sta inviando (o -1).
int store_pick_active(ts_store* s, int curHint);
// Rende attivo il titolo t (l'installazione AM sta per partire): la finestra riparte dal suo primo blocco da installare.
void store_activate(ts_store* s, int t);
int store_active(ts_store* s);
// Prossimi blocchi del titolo attivo in ordine (al massimo maxBlocks, a passo TS_BLOCK in out), dalla finestra, dal file o, se sono dichiarati disponibili ma
// ancora nella coda di parcheggio, direttamente dalla coda (ST_SRC_QUEUE: niente scrittura e rilettura sulla SD). Ritorna quanti, 0 se non ancora
// disponibili; *bytes = byte utili (l'ultimo blocco del titolo puo' essere parziale), *src = ST_SRC_*; -1 se la lettura del file fallisce.
int store_next_run(ts_store* s, uint8_t* out, uint32_t maxBlocks, size_t* bytes, int* src);
// 1 se tutti i blocchi del titolo attivo sono stati consegnati.
int store_active_complete(ts_store* s);
// Fine del titolo t (ok = installato, altrimenti errore): libera bitmap e file, torna senza titolo attivo se era lui.
void store_finish(ts_store* s, int t, int ok);

// Riepilogo dei titoli voluti non finiti (al massimo max voci). Ritorna quante.
int store_summary(ts_store* s, ts_sum* out, int max);
// Intervalli mancanti (blocchi locali) del titolo: buchi sotto il piu' alto visto e coda fino alla fine. Al massimo max intervalli; se non bastano
// l'ultimo copre tutto fino alla fine. Ritorna quanti.
int store_missing(ts_store* s, int title, ts_range* out, int max);
// AM ha preparato tutto lo spazio del titolo attivo (dopo la prima scrittura riuscita): non lo si riserva piu' in store_update_space
// (e' gia' escluso dallo spazio libero letto). Si azzera a ogni store_activate.
void store_mark_allocated(ts_store* s, int title);
// L'installatore e' a secco da un po' (aspetta un blocco che manca, non sta scrivendo in AM): la SD e' libera e il titolo attivo puo' parcheggiare anche
// cio' che arriva oltre la finestra, invece di scartarlo (un ricevente che ha perso un pezzo non deve farselo rimandare tutto dopo). 0 = no.
void store_set_starved(ts_store* s, int starved);
// Il mittente non ha piu' niente da inviare (STATE senza titolo: "fine mandata"). Da un secondo in poi nessun blocco vicino arrivera' piu' ad
// affiancarsi a quelli in coda: il parcheggio li scrive subito senza aspettare l'eta'. 0 = il mittente sta inviando. Si puo' richiamare a ogni STATE.
void store_set_idle(ts_store* s, int idle);
// Spazio che si libera quando il titolo t viene installato (la versione gia' installata si cancella prima di scrivere): entra nel calcolo di
// store_update_space. Si chiama una volta, dopo store_create.
void store_set_reclaim(ts_store* s, int title, uint64_t bytes);
// Aggiorna i titoli accettati in base allo spazio libero sulla SD (vedi PROGETTO-CAROSELLO §2.3).
void store_update_space(ts_store* s, uint64_t freeBytes);

// Stato di un titolo.
int store_state(ts_store* s, int title);
int store_accepts(ts_store* s, int title);
// Contatori (solo diagnostica).
typedef struct {
    uint64_t offered, win, parked, dup, ignored, dropFull, dropNoAccept, dropWin, fileBlocks, fileReads, parkErrors;
    uint32_t parkPeak;
    // perche' il parcheggio ha scritto (una scrittura = un passo): voce piu' vecchia con l'eta' richiesta, coda al 75%, fine mandata; eta' massima (ms) della
    // voce piu' vecchia al momento in cui e' stata scritta; blocchi che l'installatore ha preso direttamente dalla coda (senza passare dal file)
    uint64_t parkByAge, parkByValve, parkByIdle, queueBlocks;
    uint32_t parkAgeMax;
    // scansioni della coda fatte da store_next_run per prendere blocchi dichiarati disponibili ma non ancora sul file (col lock che usa anche la rete): quante,
    // quante voci in tutto, quante al massimo in una sola
    uint64_t qscanCalls, qscanElems;
    uint32_t qscanMax;
} ts_store_stats;
void store_stats(ts_store* s, ts_store_stats* out);
