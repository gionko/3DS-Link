#pragma once
#include <stddef.h>
#include <stdint.h>

// Seed dei titoli (16 byte). Il database dei seed del NAND (system save 0001000F) contiene una tabella di title ID
// seguita da una tabella di seed paralleli; qui non se ne interpreta il formato: il seed giusto si riconosce dall'hash
// che l'NCCH del titolo porta a +0x114 (primi 4 byte di SHA-256(seed || titleID little-endian)).

// 1 se `seed` e' il seed del titolo.
int seed_matches(const uint8_t seed[16], uint64_t tid, const uint8_t check[4]);

// Cerca il seed del titolo nel contenuto grezzo del database. 0 se trovato, altrimenti -1.
// `where` riceve l'offset del seed nel database.
int seed_find(const uint8_t* db, size_t size, uint64_t tid, const uint8_t check[4], uint8_t seed[16], size_t* where);
