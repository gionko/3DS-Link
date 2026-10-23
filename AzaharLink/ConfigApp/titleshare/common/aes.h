#pragma once
#include <stddef.h>
#include <stdint.h>

// AES-128 (solo cifratura) + CTR. Il contatore e' a 128 bit big-endian.
void aes_init(void);
void aes_set_key(uint32_t rk[44], const uint8_t key[16]);

// ctr0 = valore iniziale del contatore all'offset 0 del flusso. `offset` deve essere multiplo di 16.
// `len` puo' non essere multiplo di 16 solo per l'ultimo blocco del flusso.
void aes_ctr_crypt(const uint32_t rk[44], const uint8_t ctr0[16], uint64_t offset, uint8_t* data, size_t len);
