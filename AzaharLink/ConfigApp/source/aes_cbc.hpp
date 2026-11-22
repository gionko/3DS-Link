// AES-128 decryption and CBC (1.1.4, the Download Play child the app prepares with boot9.bin: analysis/piano-1.1.4-2026-11-15/specifiche/6-APP-boot9.md
// 4.1). TitleShare's aes.c (titleshare/common) only encrypts (it does CTR); taking the title key off a CIA's content needs the inverse cipher.
// The app links mbedcrypto too (ConfigApp/Makefile), which has CBC; this file is ours so that the very same code runs on the console and in the
// test container, which has no mbedtls (Tests/settings_test/dlp_guest_test.cpp builds this file as it is). Plain C++, no libctru, no heap.
// The tables are made by the compiler (constexpr): nothing is built at run time, so any thread may call these at any moment.
#pragma once

#include <cstddef>
#include <cstdint>

namespace dlpguest {

struct AesDecKey {
    std::uint32_t rk[44];  // the round keys of the equivalent inverse cipher (FIPS-197 5.3.5), first round first
};

/// The decryption round keys of a 16-byte key.
void AesDecSetKey(AesDecKey* k, const std::uint8_t key[16]);
/// One block: `in` and `out` may be the same 16 bytes.
void AesDecryptBlock(const AesDecKey& k, const std::uint8_t in[16], std::uint8_t out[16]);
/// CBC decryption in place; `len` is a multiple of 16. `iv` is left holding the last cipher block of `data`, so the next call
/// goes on with the chunk that follows (a stream decrypted in chunks gives the same bytes as one call).
void AesCbcDecrypt(const AesDecKey& k, std::uint8_t iv[16], std::uint8_t* data, std::size_t len);

} // namespace dlpguest
