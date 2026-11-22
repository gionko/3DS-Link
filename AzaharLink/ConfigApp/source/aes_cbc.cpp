#include "aes_cbc.hpp"

#include <cstring>

namespace dlpguest {

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;

constexpr u8 Xt(u8 a) {
    return static_cast<u8>((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
}

constexpr u8 Mul(u8 a, u8 b) {
    u8 r = 0;
    while (b != 0) {
        if (b & 1) {
            r = static_cast<u8>(r ^ a);
        }
        a = Xt(a);
        b = static_cast<u8>(b >> 1);
    }
    return r;
}

constexpr u8 Rotl8(u8 x, int s) {
    return static_cast<u8>((x << s) | (x >> (8 - s)));
}

struct Tables {
    u8 sbox[256];
    u8 inv[256];
    u32 td[4][256];  // the inverse round as four tables: InvMixColumns of InvSubBytes, one byte of the column each
};

// The S-box the way aes.c makes it (the same walk of the field), then its inverse and the four decryption tables
constexpr Tables MakeTables() {
    Tables t{};
    u8 p = 1, q = 1;
    do {
        p = static_cast<u8>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
        q = static_cast<u8>(q ^ (q << 1));
        q = static_cast<u8>(q ^ (q << 2));
        q = static_cast<u8>(q ^ (q << 4));
        if (q & 0x80) {
            q = static_cast<u8>(q ^ 0x09);
        }
        t.sbox[p] = static_cast<u8>(q ^ Rotl8(q, 1) ^ Rotl8(q, 2) ^ Rotl8(q, 3) ^ Rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    t.sbox[0] = 0x63;
    for (int i = 0; i < 256; ++i) {
        t.inv[t.sbox[i]] = static_cast<u8>(i);
    }
    for (int i = 0; i < 256; ++i) {
        const u8 s = t.inv[i];
        const u32 w = (static_cast<u32>(Mul(s, 0x0E)) << 24) | (static_cast<u32>(Mul(s, 0x09)) << 16) | (static_cast<u32>(Mul(s, 0x0D)) << 8) | Mul(s, 0x0B);
        t.td[0][i] = w;
        t.td[1][i] = (w >> 8) | (w << 24);
        t.td[2][i] = (w >> 16) | (w << 16);
        t.td[3][i] = (w >> 24) | (w << 8);
    }
    return t;
}

constexpr Tables kT = MakeTables();
static_assert(kT.sbox[0x00] == 0x63 && kT.sbox[0x01] == 0x7C && kT.sbox[0x53] == 0xED && kT.sbox[0xFF] == 0x16, "the S-box of FIPS-197 figure 7");
static_assert(kT.inv[0x63] == 0x00 && kT.inv[0xED] == 0x53, "the inverse S-box");

inline u32 Load(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3];
}

inline void Store(u8* p, u32 w) {
    p[0] = static_cast<u8>(w >> 24);
    p[1] = static_cast<u8>(w >> 16);
    p[2] = static_cast<u8>(w >> 8);
    p[3] = static_cast<u8>(w);
}

inline u32 InvMixColumn(u32 w) {
    return kT.td[0][kT.sbox[w >> 24]] ^ kT.td[1][kT.sbox[(w >> 16) & 0xFF]] ^ kT.td[2][kT.sbox[(w >> 8) & 0xFF]] ^ kT.td[3][kT.sbox[w & 0xFF]];
}

} // namespace

void AesDecSetKey(AesDecKey* k, const u8 key[16]) {
    u32 ek[44];
    for (int i = 0; i < 4; ++i) {
        ek[i] = Load(key + 4 * i);
    }
    u8 rcon = 1;
    for (int i = 4; i < 44; ++i) {
        u32 t = ek[i - 1];
        if (i % 4 == 0) {
            t = (t << 8) | (t >> 24);
            t = (static_cast<u32>(kT.sbox[t >> 24]) << 24) | (static_cast<u32>(kT.sbox[(t >> 16) & 0xFF]) << 16) |
                (static_cast<u32>(kT.sbox[(t >> 8) & 0xFF]) << 8) | kT.sbox[t & 0xFF];
            t ^= static_cast<u32>(rcon) << 24;
            rcon = Xt(rcon);
        }
        ek[i] = ek[i - 4] ^ t;
    }
    // the equivalent inverse cipher: the encryption keys backwards, the middle ones through InvMixColumns
    for (int j = 0; j < 4; ++j) {
        k->rk[j] = ek[40 + j];
        k->rk[40 + j] = ek[j];
    }
    for (int r = 1; r < 10; ++r) {
        for (int j = 0; j < 4; ++j) {
            k->rk[4 * r + j] = InvMixColumn(ek[4 * (10 - r) + j]);
        }
    }
    volatile u32* wipe = ek;  // (the expanded key of a title key: not left on the stack)
    for (int i = 0; i < 44; ++i) {
        wipe[i] = 0;
    }
}

void AesDecryptBlock(const AesDecKey& k, const u8 in[16], u8 out[16]) {
    const u32* rk = k.rk;
    u32 s0 = Load(in) ^ rk[0], s1 = Load(in + 4) ^ rk[1], s2 = Load(in + 8) ^ rk[2], s3 = Load(in + 12) ^ rk[3];
    for (int r = 1; r < 10; ++r) {
        rk += 4;
        const u32 t0 = kT.td[0][s0 >> 24] ^ kT.td[1][(s3 >> 16) & 0xFF] ^ kT.td[2][(s2 >> 8) & 0xFF] ^ kT.td[3][s1 & 0xFF] ^ rk[0];
        const u32 t1 = kT.td[0][s1 >> 24] ^ kT.td[1][(s0 >> 16) & 0xFF] ^ kT.td[2][(s3 >> 8) & 0xFF] ^ kT.td[3][s2 & 0xFF] ^ rk[1];
        const u32 t2 = kT.td[0][s2 >> 24] ^ kT.td[1][(s1 >> 16) & 0xFF] ^ kT.td[2][(s0 >> 8) & 0xFF] ^ kT.td[3][s3 & 0xFF] ^ rk[2];
        const u32 t3 = kT.td[0][s3 >> 24] ^ kT.td[1][(s2 >> 16) & 0xFF] ^ kT.td[2][(s1 >> 8) & 0xFF] ^ kT.td[3][s0 & 0xFF] ^ rk[3];
        s0 = t0;
        s1 = t1;
        s2 = t2;
        s3 = t3;
    }
    rk += 4;
    const auto last = [](u32 a, u32 b, u32 c, u32 d) {
        return (static_cast<u32>(kT.inv[a >> 24]) << 24) | (static_cast<u32>(kT.inv[(b >> 16) & 0xFF]) << 16) |
               (static_cast<u32>(kT.inv[(c >> 8) & 0xFF]) << 8) | kT.inv[d & 0xFF];
    };
    Store(out, last(s0, s3, s2, s1) ^ rk[0]);
    Store(out + 4, last(s1, s0, s3, s2) ^ rk[1]);
    Store(out + 8, last(s2, s1, s0, s3) ^ rk[2]);
    Store(out + 12, last(s3, s2, s1, s0) ^ rk[3]);
}

void AesCbcDecrypt(const AesDecKey& k, u8 iv[16], u8* data, std::size_t len) {
    u8 cipher[16], plain[16];
    for (std::size_t at = 0; at + 16 <= len; at += 16) {
        std::memcpy(cipher, data + at, 16);
        AesDecryptBlock(k, cipher, plain);
        for (int i = 0; i < 16; ++i) {
            data[at + i] = static_cast<u8>(plain[i] ^ iv[i]);
        }
        std::memcpy(iv, cipher, 16);  // the IV of the next block (and of the next call): this cipher block
    }
}

} // namespace dlpguest
