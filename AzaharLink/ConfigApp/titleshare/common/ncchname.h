#pragma once
#include <stddef.h>
#include <stdint.h>

// Nome commerciale di un titolo installato sulla SD, letto dall'icona (SMDH) dentro l'ExeFS del suo NCCH principale,
// decifrando lo strato SD e lo strato NCCH con le chiavi dell'utente (boot9.bin + movable.sed).
// ctrRoot: "sdmc:/Nintendo 3DS/<ID0>/<ID1>"; sdrk: chiave dello strato SD gia' espansa; keyX: KeyX slot 0x2C.
// language: indice della lingua di sistema (0 = JP, 1 = EN, 2 = FR, 3 = DE, 4 = IT, 5 = ES, ...).
// Ritorna 0 se riuscito; un numero negativo se il titolo non ha icona (DLC) o non si legge.
int ncch_read_name(const uint32_t sdrk[44], const uint8_t keyX[16], const char* ctrRoot, uint64_t tid, int language, char* out, size_t outSize);
