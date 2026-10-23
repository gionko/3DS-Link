#pragma once
#include <stddef.h>
#include <stdint.h>

// Strato di cifratura SD del 3DS (AES-CTR sotto Nintendo 3DS/<ID0>/<ID1>/).
// Le chiavi non sono nel codice: KeyX viene letta da boot9.bin, KeyY da movable.sed (entrambi dell'utente).

// boot9 puo' essere il dump da 0x10000 o da 0x8000 byte. Ritorna 0 se riuscito.
int sd_derive_key(const uint8_t* boot9, size_t boot9Size, const uint8_t* movable, size_t movableSize, uint8_t key[16]);

// Nome della cartella ID0 (32 caratteri esadecimali + NUL) ricavato da movable.sed.
void sd_id0(const uint8_t* movable, char out[33]);

// Contatore iniziale per un percorso relativo alla cartella ID1, es. "/title/00040000/00030200/content/00000000.app".
void sd_path_ctr(const char* path, uint8_t ctr[16]);

// Chiave dello strato NCCH (ExeFS header, icona, banner): KeyX slot 0x2C da boot9.bin, KeyY = primi 16 byte della
// firma dell'intestazione NCCH. sd_ncch_keyx ritorna 0 se riuscito.
int sd_ncch_keyx(const uint8_t* boot9, size_t boot9Size, uint8_t x[16]);
void ncch_scramble(const uint8_t x[16], const uint8_t y[16], uint8_t key[16]);
