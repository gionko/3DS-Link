#include "azl/crypto.hpp"

namespace azl {

// ---------------------------------------------------------------------------------------------
// MD5 (RFC 1321)
// ---------------------------------------------------------------------------------------------
namespace {

inline u32 Rotl(u32 x, int c) {
    return (x << c) | (x >> (32 - c));
}

const u32 kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

const int kMd5S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                       5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                       6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

void Md5Block(u32 state[4], const u8* block) {
    u32 m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = LoadLE32(block + i * 4);
    }
    u32 a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; ++i) {
        u32 f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        const u32 tmp = d;
        d = c;
        c = b;
        b = b + Rotl(a + f + kMd5K[i] + m[g], kMd5S[i]);
        a = tmp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

// Shared Merkle-Damgard padding: appends 0x80, zeros and the bit length (LE for MD5, BE for SHA-1).
template <typename BlockFn>
void Pad(const u8* data, std::size_t len, bool length_big_endian, BlockFn block_fn) {
    std::size_t pos = 0;
    while (len - pos >= 64) {
        block_fn(data + pos);
        pos += 64;
    }
    u8 tail[128] = {};
    const std::size_t rem = len - pos;
    if (rem) {
        std::memcpy(tail, data + pos, rem);
    }
    tail[rem] = 0x80;
    const std::size_t total = (rem + 1 + 8 <= 64) ? 64 : 128;
    const u64 bits = static_cast<u64>(len) * 8;
    if (length_big_endian) {
        StoreBE64(tail + total - 8, bits);
    } else {
        StoreLE64(tail + total - 8, bits);
    }
    block_fn(tail);
    if (total == 128) {
        block_fn(tail + 64);
    }
}

} // namespace

void Md5(const u8* data, std::size_t len, u8 out[16]) {
    u32 state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    Pad(data, len, false, [&](const u8* block) { Md5Block(state, block); });
    for (int i = 0; i < 4; ++i) {
        StoreLE32(out + i * 4, state[i]);
    }
}

// ---------------------------------------------------------------------------------------------
// SHA-1
// ---------------------------------------------------------------------------------------------
namespace {

void Sha1Block(u32 h[5], const u8* block) {
    u32 w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = LoadBE32(block + i * 4);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = Rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
        u32 f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const u32 tmp = Rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = Rotl(b, 30);
        b = a;
        a = tmp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

} // namespace

void Sha1(const u8* data, std::size_t len, u8 out[20]) {
    u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    Pad(data, len, true, [&](const u8* block) { Sha1Block(h, block); });
    for (int i = 0; i < 5; ++i) {
        StoreBE32(out + i * 4, h[i]);
    }
}

// ---------------------------------------------------------------------------------------------
// AES-128 (encrypt direction only, which is all CTR mode needs)
// ---------------------------------------------------------------------------------------------
namespace {

struct AesTables {
    u8 sbox[256];
    AesTables() {
        // Generate the S-box: multiplicative inverse in GF(2^8) followed by the affine transform.
        u8 p = 1, q = 1;
        do {
            // p *= 3
            p = static_cast<u8>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
            // q /= 3
            q ^= static_cast<u8>(q << 1);
            q ^= static_cast<u8>(q << 2);
            q ^= static_cast<u8>(q << 4);
            if (q & 0x80) {
                q ^= 0x09;
            }
            const u8 x = static_cast<u8>(q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^
                                         (q << 3 | q >> 5) ^ (q << 4 | q >> 4));
            sbox[p] = static_cast<u8>(x ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
    }
};

const AesTables& Tables() {
    static const AesTables tables;
    return tables;
}

inline u8 XTime(u8 x) {
    return static_cast<u8>((x << 1) ^ ((x & 0x80) ? 0x1B : 0));
}

void AesExpandKey(const u8 key[16], u8 round_keys[176]) {
    const u8* sbox = Tables().sbox;
    std::memcpy(round_keys, key, 16);
    u8 rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        u8 t[4] = {round_keys[i - 4], round_keys[i - 3], round_keys[i - 2], round_keys[i - 1]};
        if (i % 16 == 0) {
            const u8 first = t[0];
            t[0] = static_cast<u8>(sbox[t[1]] ^ rcon);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[first];
            rcon = XTime(rcon);
        }
        for (int j = 0; j < 4; ++j) {
            round_keys[i + j] = static_cast<u8>(round_keys[i - 16 + j] ^ t[j]);
        }
    }
}

void AesEncryptBlock(const u8 round_keys[176], const u8 in[16], u8 out[16]) {
    const u8* sbox = Tables().sbox;
    u8 s[16];
    for (int i = 0; i < 16; ++i) {
        s[i] = static_cast<u8>(in[i] ^ round_keys[i]);
    }
    for (int round = 1; round <= 10; ++round) {
        u8 t[16];
        // SubBytes + ShiftRows
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                t[c * 4 + r] = sbox[s[((c + r) & 3) * 4 + r]];
            }
        }
        if (round != 10) {
            // MixColumns
            for (int c = 0; c < 4; ++c) {
                const u8 a0 = t[c * 4], a1 = t[c * 4 + 1], a2 = t[c * 4 + 2], a3 = t[c * 4 + 3];
                s[c * 4 + 0] = static_cast<u8>(XTime(a0) ^ (XTime(a1) ^ a1) ^ a2 ^ a3);
                s[c * 4 + 1] = static_cast<u8>(a0 ^ XTime(a1) ^ (XTime(a2) ^ a2) ^ a3);
                s[c * 4 + 2] = static_cast<u8>(a0 ^ a1 ^ XTime(a2) ^ (XTime(a3) ^ a3));
                s[c * 4 + 3] = static_cast<u8>((XTime(a0) ^ a0) ^ a1 ^ a2 ^ XTime(a3));
            }
        } else {
            std::memcpy(s, t, 16);
        }
        for (int i = 0; i < 16; ++i) {
            s[i] = static_cast<u8>(s[i] ^ round_keys[round * 16 + i]);
        }
    }
    std::memcpy(out, s, 16);
}

} // namespace

void Aes128CtrXor(const u8 key[16], const u8 counter[16], u8* data, std::size_t len) {
    u8 round_keys[176];
    AesExpandKey(key, round_keys);

    u8 ctr[16];
    std::memcpy(ctr, counter, 16);
    for (std::size_t off = 0; off < len; off += 16) {
        u8 keystream[16];
        AesEncryptBlock(round_keys, ctr, keystream);
        const std::size_t n = (len - off < 16) ? (len - off) : 16;
        for (std::size_t i = 0; i < n; ++i) {
            data[off + i] ^= keystream[i];
        }
        for (int i = 15; i >= 0; --i) { // big-endian increment
            if (++ctr[i] != 0) {
                break;
            }
        }
    }
}

} // namespace azl
