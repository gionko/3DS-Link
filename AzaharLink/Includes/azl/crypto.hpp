// Minimal MD5 / SHA-1 / AES-128-CTR, used for the UDS beacon payload only.
// (Azahar uses an all-zero key and these exact algorithms.)
#pragma once

#include "azl/common.hpp"

namespace azl {

void Md5(const u8* data, std::size_t len, u8 out[16]);
void Sha1(const u8* data, std::size_t len, u8 out[20]);

// Encrypts/decrypts `data` in place with AES-128-CTR. `counter` is the initial 16-byte counter
// block, incremented as a big-endian integer (same as Crypto++ CTR_Mode).
void Aes128CtrXor(const u8 key[16], const u8 counter[16], u8* data, std::size_t len);

} // namespace azl
