#pragma once
#include <stdint.h>

// Dimensione di un buffer ricavata da una percentuale della memoria libera. Codice puro: provato su PC (share/tests/mem_test.c).

// Quanti blocchi da `bytesPerBlock` byte stanno in `pct`% di `freeBytes`, arrotondati per difetto a un multiplo di `unit` e
// limitati a `hi`. 0 se non ne stanno almeno `lo`.
static inline uint32_t ts_budget_blocks(uint64_t freeBytes, uint32_t pct, uint32_t bytesPerBlock, uint32_t unit, uint32_t lo, uint32_t hi) {
    if(bytesPerBlock == 0 || unit == 0) {
        return 0;
    }

    uint64_t b = freeBytes * pct / 100 / bytesPerBlock;
    if(b > hi) {
        b = hi;
    }

    b -= b % unit;
    return b < lo ? 0 : (uint32_t) b;
}
