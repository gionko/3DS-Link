#pragma once
#include <stddef.h>
#include <stdint.h>

#include "proto.h"

// Finestra di ricezione di un ricevente: tiene i blocchi arrivati fuori ordine finche' non e' possibile consegnarli
// in ordine. Un solo thread inserisce (win_put) e un solo thread consuma (win_peek/win_pop).

typedef struct {
    uint32_t W;
    uint8_t* data;          // W * TS_BLOCK byte
    uint16_t* len;          // lunghezza dei dati di ogni slot
    volatile uint8_t* have; // 1 se lo slot contiene il blocco atteso
    volatile uint32_t cursor; // primo blocco non ancora consegnato
    volatile uint32_t hi;     // ultimo blocco ricevuto + 1
    uint64_t statRecv, statDup, statOut;
    // Raffiche di blocchi saltati: un blocco che arriva con numero maggiore di `hi` lascia un buco di (block - hi) blocchi.
    uint64_t statGaps, statGapBlocks;
    uint32_t statGapMax;
    uint32_t gapHist[5]; // lunghezza del buco: 1, 2-3, 4-15, 16-127, 128 o piu'
} ts_win;

int win_init(ts_win* w, uint32_t window);
void win_free(ts_win* w);
// Riparte da `cursor`: svuota la finestra (nessun blocco presente), cursore e hi a `cursor`. Si usa quando cambia il titolo attivo.
void win_reset(ts_win* w, uint32_t cursor);

// 1 = memorizzato, 0 = gia' consegnato o doppio, -1 = fuori finestra.
int win_put(ts_win* w, uint32_t block, const uint8_t* payload, size_t len);

// Blocco al cursore, se presente (altrimenti NULL).
const uint8_t* win_peek(ts_win* w, size_t* len);

// Consegna il blocco al cursore e libera lo slot.
void win_pop(ts_win* w);

// Intervalli mancanti tra il cursore e `hi`. Ritorna il numero di intervalli scritti.
int win_missing(const ts_win* w, ts_range* out, int max);

// Salta un intervallo di blocchi che non arrivera' mai (titoli che nessuno ha chiesto). Il cursore deve essere `from`.
void win_skip(ts_win* w, uint32_t from, uint32_t to);

// Come win_missing, ma considera presenti i blocchi negli intervalli `skip` (ordinati, non sovrapposti).
int win_missing_skip(const ts_win* w, const ts_range* skip, int nSkip, ts_range* out, int max);
