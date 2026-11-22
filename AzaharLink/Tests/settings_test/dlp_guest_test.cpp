// The Download Play child the app prepares with boot9.bin (1.1.4, analysis/piano-1.1.4-2026-11-15/specifiche/6-APP-boot9.md 9, piece APP-CORE): the pure
// part on the PC, with the app's own files built as they are (ConfigApp/source/aes_cbc.cpp, dlp_guest_logic.hpp, dlp_guest_compose.cpp) and TitleShare's
// aes, sha256, sdcrypt and the built-in values (builtin_keys.c). What it pins:
//  * AES: FIPS-197 C.1 and SP 800-38A F.2.2 decrypted, any key against aes.c's encryption, CBC carried across chunks of 16 B and 4 KB + 16;
//  * the key generator against sdcrypt.c's ncch_scramble with the real constant, and boot9.bin's two KeyX (whole and protected half; any other size refused);
//  * the whole chain on made-up children (Tests/settings_test/dlp_guest_synth.hpp: no byte of Nintendo, keys made up), streamed in chunks of 16 B, 4 KB + 16
//    and 256 KB, found in a RomFS by FindChildPackage as the console will: the output read back by a parser of this file - the length, the TMD's chain, its
//    type and hash, NoCrypto, the partition kept, the four ids and 0x160, the ExeFS and the RomFS in the clear, the certificates, the ticket's id; hidden:
//    the icon's bit off in the ExeFS and in the meta block, the two hashes made again;
//  * the refusals: a damaged content (kErrSourceHash, nothing reaches the AM sink), a wrong KeyY (kErrTitleKey before the stream: no byte of the work
//    file), every bad layout, ticket and TMD, the NCCH's methods, seed, fixed key and exheader size, a child with no icon (kErrCannotHide), kFixCrr;
//  * every result has its words;
//  * (the build 2e slot25) method 1: a made-up child whose secondary key is a made-up KeyX of "slot 0x25", found in a made-up boot.firm with false
//    candidates around it (dlp_guest_synth.hpp MakeFirm: NEVER a real key, in no test): found in the ARM9's section or in another one, not found,
//    no boot.firm, a boot.firm with a wrong hash or no "FIRM", no RomFS, 0x1E0 or .code's hash wrong; the line of the log, never the key in it;
//  * TestRealFiles: with the private files of the working tree (the child of Island Tour and boot9.bin, read where they are, never copied, never printed)
//    the app's composer makes, byte for byte, the PC's V2vis (visible: the only use of kVisible) and V2 (hidden) by their md5: P2a and P2n in the form
//    makerom gives an application of the SD card (the build 2e, analysis/piano-1.1.4-2026-11-15/home-figlio-2026-11-18/varianti). Without them - the
//    published tree - it says so loudly and is skipped.
// Built with -DDLPGUEST_TEST_VISIBLE (Tests/run.sh): the console's build refuses a visible child outright.
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "aes_cbc.hpp"
#include "azl/dlp_title_block.hpp"
#include "azl/title_rules.hpp"
#include "dlp_guest_compose.hpp"
#include "dlp_guest_logic.hpp"
#include "dlp_guest_synth.hpp"

extern "C" {
#include "../../ConfigApp/titleshare/common/sdcrypt.h"
#include "../../ConfigApp/titleshare_host/builtin_keys.h"
}

using namespace dlpguest;
using synth::Bytes;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
        }                                                                                          \
    } while (0)
#define CHECK_EQ(a, b)                                                                                                        \
    do {                                                                                                                      \
        ++g_checks;                                                                                                           \
        const long long va_ = static_cast<long long>(a), vb_ = static_cast<long long>(b);                                     \
        if (va_ != vb_) {                                                                                                     \
            ++g_failures;                                                                                                     \
            std::fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_);            \
        }                                                                                                                     \
    } while (0)

static Bytes Hex(const char* h) {
    Bytes b;
    for (; h[0] && h[1]; h += 2) {
        unsigned v = 0;
        std::sscanf(h, "%2x", &v);
        b.push_back(static_cast<u8>(v));
    }
    return b;
}

static std::string ToHex(const u8* p, std::size_t n) {
    std::string s;
    char t[3];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(t, sizeof(t), "%02x", p[i]);
        s += t;
    }
    return s;
}

// ---- MD5 (RFC 1321), for the md5 of the PC's oracles --------------------------------------------------------------------------------

struct Md5 {
    u32 h[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    u64 total = 0;
    u8 buf[64];
    std::size_t used = 0;
    static u32 Rol(u32 x, int c) {
        return (x << c) | (x >> (32 - c));
    }
    void Block(const u8* p) {
        static const u32 K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
            0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
            0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
            0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
            0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
            0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        u32 m[16];
        for (int i = 0; i < 16; ++i) {
            m[i] = static_cast<u32>(p[4 * i]) | (static_cast<u32>(p[4 * i + 1]) << 8) | (static_cast<u32>(p[4 * i + 2]) << 16) | (static_cast<u32>(p[4 * i + 3]) << 24);
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            u32 f;
            int g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            const u32 t = d;
            d = c;
            c = b;
            b = b + Rol(a + f + K[i] + m[g], R[i]);
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
    }
    void Update(const u8* p, std::size_t n) {
        total += n;
        while (n > 0) {
            const std::size_t k = (64 - used) < n ? (64 - used) : n;
            std::memcpy(buf + used, p, k);
            used += k;
            p += k;
            n -= k;
            if (used == 64) {
                Block(buf);
                used = 0;
            }
        }
    }
    std::string Hex() {
        const u64 bits = total * 8;
        const u8 one = 0x80, zero = 0;
        Update(&one, 1);
        while (used != 56) {
            Update(&zero, 1);
        }
        u8 len[8];
        for (int i = 0; i < 8; ++i) {
            len[i] = static_cast<u8>(bits >> (8 * i));
        }
        Update(len, 8);
        u8 out[16];
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                out[4 * i + j] = static_cast<u8>(h[i] >> (8 * j));
            }
        }
        return ToHex(out, 16);
    }
};

// ---- the I/O the composer is given: memory here ------------------------------------------------------------------------------------

struct Mem {
    const Bytes* data;
    u32 reads = 0;
    static bool Read(void* ctx, u64 off, u8* buf, u32 len) {
        Mem& m = *static_cast<Mem*>(ctx);
        ++m.reads;
        if (off > m.data->size() || len > m.data->size() - off) {
            return false;
        }
        std::memcpy(buf, m.data->data() + off, len);
        return true;
    }
};

struct Temp {
    Bytes bytes;
    u64 writes = 0, written = 0;
    bool in_order = true;
    static bool Write(void* ctx, u64 off, const u8* buf, u32 len) {
        Temp& t = *static_cast<Temp*>(ctx);
        t.in_order = t.in_order && off == t.bytes.size();
        if (t.bytes.size() < off + len) {
            t.bytes.resize(static_cast<std::size_t>(off + len));
        }
        std::memcpy(t.bytes.data() + off, buf, len);
        ++t.writes;
        t.written += len;
        return true;
    }
    static bool Read(void* ctx, u64 off, u8* buf, u32 len) {
        Temp& t = *static_cast<Temp*>(ctx);
        if (off > t.bytes.size() || len > t.bytes.size() - off) {
            return false;
        }
        std::memcpy(buf, t.bytes.data() + off, len);
        return true;
    }
};

struct Sink {  // the AM sink: sequential
    Bytes bytes;
    u32 calls = 0;
    bool in_order = true;
    Md5 md5;
    bool keep = true;
    static bool Write(void* ctx, u64 off, const u8* buf, u32 len) {
        Sink& s = *static_cast<Sink*>(ctx);
        ++s.calls;
        s.in_order = s.in_order && off == s.md5.total;
        s.md5.Update(buf, len);
        if (s.keep) {
            s.bytes.insert(s.bytes.end(), buf, buf + len);
        }
        return true;
    }
};

static Keys KeysOf(const synth::MadeKeys& k) {
    Keys out;
    std::memcpy(out.ncch_x, k.ncch_x, 16);
    std::memcpy(out.common_x, k.common_x, 16);
    std::memcpy(out.common_y0, k.common_y0, 16);
    std::memcpy(out.generator, k.generator, 16);
    return out;
}

struct Run {
    int compose = -1, emit = -1;
    Temp temp;
    Sink sink;
    u64 temp_writes_before_stream = 0;
    Slot25Step slot25 = Slot25Step::None;  // (the build 2e slot25) what Composed said of the search in boot.firm
    u32 slot25_at = 0, candidates = 0;
    int firm_asks = 0;                     // how many times boot.firm was asked for
    std::string line;                      // CryptoLine of the child (game 00040000000F8200)
};

// (the review of DPAPP-2e slot25) a made-up clock for the line's time: +3 ms at every reading, +1000 for a read of boot.firm (FirmSource)
static u64 g_now = 0;
static bool g_clock = false;
static u64 TestClock() {
    return g_now += 3;
}

// boot.firm as the run gives it: the bytes, or none
struct FirmSource {
    const Bytes* bytes;
    int asks = 0;
    static bool Get(void* ctx, const u8** data, std::size_t* size) {
        FirmSource& f = *static_cast<FirmSource*>(ctx);
        ++f.asks;
        g_now += 1000;  // (a slow SD card)
        if (f.bytes == nullptr) {
            return false;
        }
        *data = f.bytes->data();
        *size = f.bytes->size();
        return true;
    }
};

static bool CancelAt(void* ctx, int stage, u64 done, u64 total) {
    (void) total;
    return !(stage == kStageDecrypt && done >= *static_cast<u64*>(ctx));
}

/// Composes the CIA at `cia_at` of `image` and, only when that worked (the worker's order), emits it into the sink.
static Run Compose1(const Bytes& image, u64 cia_at, u64 cia_size, const Keys& keys, u64 prepared, u8 variant, std::size_t chunk, StepFn step = nullptr,
                    void* sctx = nullptr, const Bytes* firm = nullptr, u64 am_title = 0) {
    Run r;
    Mem src{&image};
    FirmSource fs{firm};
    std::vector<u8> buf(chunk), work(kWorkBytes);
    ComposeIn in;
    in.read = &Mem::Read;
    in.rctx = &src;
    in.cia_at = cia_at;
    in.cia_size = cia_size;
    in.keys = &keys;
    in.prepared = prepared;
    in.am_title = am_title;  // (the build 2f: 0 = the prepared id, the old form)
    in.variant = variant;
    in.temp = &Temp::Write;
    in.tctx = &r.temp;
    in.step = step;
    in.sctx = sctx;
    in.buf = buf.data();
    in.buf_size = buf.size();
    in.work = work.data();
    in.work_size = work.size();
    if (firm != nullptr) {
        in.firm = &FirmSource::Get;
        in.fctx = &fs;
    }
    if (g_clock) {
        in.clock_ms = &TestClock;
    }
    Composed* c = new Composed;  // (on the heap, as on the console)
    r.compose = Compose(in, c);
    r.slot25 = c->slot25;
    r.slot25_at = c->slot25_at;
    r.candidates = c->slot25_candidates;
    r.firm_asks = fs.asks;
    char line[kCryptoLineBytes];
    CryptoLine(*c, 0x00040000000F8200ull, line, sizeof(line));
    r.line = line;
    if (r.compose == 0) {
        std::vector<u8> out(chunk + 7);  // (another size than the stream's)
        r.emit = Emit(*c, &Temp::Read, &r.temp, &Sink::Write, &r.sink, nullptr, nullptr, out.data(), out.size());
    }
    delete c;
    return r;
}

// ---- 1. AES -------------------------------------------------------------------------------------------------------------------------

static void TestAes() {
    std::printf("1: AES-128 decryption and CBC\n");
    aes_init();  // (aes.c's tables, for its encryption below; on the console bk_get readies them before any Compose)
    // FIPS-197 appendix C.1
    {
        const Bytes key = Hex("000102030405060708090a0b0c0d0e0f"), cipher = Hex("69c4e0d86a7b0430d8cdb78070b4c55a");
        AesDecKey k;
        AesDecSetKey(&k, key.data());
        u8 out[16];
        AesDecryptBlock(k, cipher.data(), out);
        CHECK(ToHex(out, 16) == "00112233445566778899aabbccddeeff");
        u8 same[16];
        std::memcpy(same, cipher.data(), 16);
        AesDecryptBlock(k, same, same);  // in place
        CHECK(std::memcmp(same, out, 16) == 0);
    }
    // SP 800-38A F.2.2, CBC-AES128.Decrypt
    {
        const Bytes key = Hex("2b7e151628aed2a6abf7158809cf4f3c");
        Bytes iv = Hex("000102030405060708090a0b0c0d0e0f");
        Bytes data = Hex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b273bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7");
        AesDecKey k;
        AesDecSetKey(&k, key.data());
        AesCbcDecrypt(k, iv.data(), data.data(), data.size());
        CHECK(ToHex(data.data(), data.size()) ==
              "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
        CHECK(ToHex(iv.data(), 16) == "3ff1caa1681fac09120eca307586e1a7");  // the IV left for the next chunk: the last cipher block
    }
    // any key: the inverse of aes.c's encryption
    synth::Rng r(77);
    for (int t = 0; t < 200; ++t) {
        u8 key[16], p[16], c[16], back[16];
        r.Fill(key, 16);
        r.Fill(p, 16);
        u32 rk[44];
        aes_set_key(rk, key);
        synth::EncBlock(rk, p, c);
        AesDecKey k;
        AesDecSetKey(&k, key);
        AesDecryptBlock(k, c, back);
        CHECK(std::memcmp(back, p, 16) == 0);
    }
    // the chain across chunks
    for (std::size_t chunk : {std::size_t(16), std::size_t(4096 + 16), std::size_t(0x10000)}) {
        u8 key[16], iv0[16];
        r.Fill(key, 16);
        r.Fill(iv0, 16);
        Bytes plain(0x10000 + 0x30);
        r.Fill(plain.data(), plain.size());
        Bytes c = plain;
        synth::CbcEncrypt(key, iv0, c.data(), c.size());
        AesDecKey k;
        AesDecSetKey(&k, key);
        u8 iv[16];
        std::memcpy(iv, iv0, 16);
        for (std::size_t at = 0; at < c.size(); at += chunk) {
            AesCbcDecrypt(k, iv, c.data() + at, std::min(chunk, c.size() - at));
        }
        CHECK(c == plain);
    }
}

// ---- 2-3. the key generator and boot9 ------------------------------------------------------------------------------------------------

static void TestKeys() {
    std::printf("2: the key generator against sdcrypt.c, boot9's two KeyX\n");
    uint8_t c[16];
    CHECK(bk_get(BK_GENERATOR_CONSTANT, c) == 0);
    synth::Rng r(5);
    for (int t = 0; t < 100; ++t) {
        u8 x[16], y[16], mine[16], theirs[16], other[16];
        r.Fill(x, 16);
        r.Fill(y, 16);
        Scramble(x, y, c, mine);
        ncch_scramble(x, y, theirs);  // (sdcrypt.c, the real constant through bk_get)
        CHECK(std::memcmp(mine, theirs, 16) == 0);
        u8 c2[16];
        r.Fill(c2, 16);
        Scramble(x, y, c2, mine);
        synth::KeyOf(x, y, c2, other);  // (any constant: the generator's own 128-bit arithmetic)
        CHECK(std::memcmp(mine, other, 16) == 0);
    }
    // boot9.bin: made up, every byte its own
    Bytes b9(kBoot9Whole);
    for (std::size_t i = 0; i < b9.size(); ++i) {
        b9[i] = static_cast<u8>(i * 7 + (i >> 8));
    }
    Keys k;
    std::memset(&k, 0xEE, sizeof(k));
    CHECK(KeysFromBoot9(b9.data(), b9.size(), &k));
    CHECK(std::memcmp(k.ncch_x, &b9[0x8000 + 0x59D0], 16) == 0);
    CHECK(std::memcmp(k.common_x, &b9[0x8000 + 0x5A20], 16) == 0);
    u8 sd_x[16];
    CHECK(sd_ncch_keyx(b9.data(), b9.size(), sd_x) == 0 && std::memcmp(sd_x, k.ncch_x, 16) == 0);  // the same KeyX 0x2C as the SD reader's
    CHECK(k.common_y0[0] == 0xEE && k.generator[15] == 0xEE);  // (the built-in two are not boot9's)
    Bytes half(b9.begin() + 0x8000, b9.end());
    Keys h;
    CHECK(KeysFromBoot9(half.data(), half.size(), &h));
    CHECK(std::memcmp(h.ncch_x, k.ncch_x, 16) == 0 && std::memcmp(h.common_x, k.common_x, 16) == 0);
    Bytes odd(0x8400);
    CHECK(!KeysFromBoot9(odd.data(), odd.size(), &h));
    CHECK(!KeysFromBoot9(b9.data(), 0x10001, &h));
    CHECK(!KeysFromBoot9(nullptr, kBoot9Whole, &h));
    // ComposeKeys: boot9's two and the built-in two
    Keys all;
    CHECK(ComposeKeys(b9.data(), b9.size(), &all) == 0);
    CHECK(std::memcmp(all.ncch_x, k.ncch_x, 16) == 0 && std::memcmp(all.generator, c, 16) == 0);
    uint8_t y0[16];
    CHECK(bk_get(BK_COMMON_KEY_Y0, y0) == 0 && std::memcmp(all.common_y0, y0, 16) == 0);
    Keys none;
    CHECK(ComposeKeys(odd.data(), odd.size(), &none) == kErrBoot9NoKeys);
    const u8 zero[sizeof(Keys)] = {0};
    CHECK(std::memcmp(&none, zero, sizeof(none)) == 0);
    Wipe(&all, sizeof(all));
    CHECK(std::memcmp(&all, zero, sizeof(all)) == 0);
}

// ---- 4. ids --------------------------------------------------------------------------------------------------------------------------

static void TestIds() {
    std::printf("4: the prepared id\n");
    CHECK(azl::title_rules::PreparedOf(0x00040000000F8200ull) == 0x000400000F8F8200ull);
    CHECK(azl::title_rules::PreparedOf(0x000400000003D100ull) == 0);  // (3DS Link's own id: no child possible, the worker says kErrIdIsApp)
    CHECK(std::string(Phrase(kErrIdIsApp)) != Phrase(-1));
}

// ---- the parser of the output (independent: nothing of dlp_guest_logic.hpp) --------------------------------------------------------

static u32 L32(const Bytes& b, u64 at) {
    return static_cast<u32>(b[at]) | (static_cast<u32>(b[at + 1]) << 8) | (static_cast<u32>(b[at + 2]) << 16) | (static_cast<u32>(b[at + 3]) << 24);
}
static u64 L64(const Bytes& b, u64 at) {
    return static_cast<u64>(L32(b, at)) | (static_cast<u64>(L32(b, at + 4)) << 32);
}
static u64 B64(const Bytes& b, u64 at) {
    u64 v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | b[at + i];
    }
    return v;
}
static u16 B16(const Bytes& b, u64 at) {
    return static_cast<u16>((b[at] << 8) | b[at + 1]);
}
static bool ShaIs(const Bytes& b, u64 at, std::size_t n, const Bytes& where, u64 hash_at) {
    u8 h[32];
    sha256(b.data() + at, n, h);
    return std::memcmp(h, where.data() + hash_at, 32) == 0;
}

/// The output of a made-up child, every part looked at.
// `am` (the build 2f): the id AM installs the title by - the ticket, the TMD, 0x108 and 0x118; 0 = `prepared` (the old form). The exheader's ids
// (0x3C8, 0x400, 0x800) are `prepared` in both forms.
static void CheckOutput(const synth::Made& m, const Bytes& out, u64 prepared, bool hidden, u64 am = 0) {
    if (am == 0) {
        am = prepared;
    }
    CHECK_EQ(out.size(), m.cia.size());
    if (out.size() != m.cia.size()) {
        return;
    }
    CHECK(std::memcmp(out.data(), m.cia.data(), 0x2020) == 0);  // the header
    CHECK(std::memcmp(out.data() + m.cert_at, m.cert.data(), m.cert.size()) == 0);
    // the ticket: only its title id
    const u64 bt = m.ticket_at + 4 + 0x13C;
    CHECK(B64(out, bt + 0x9C) == am);
    for (u64 i = 0; i < m.ticket.size(); ++i) {
        if (i < bt - m.ticket_at + 0x9C || i >= bt - m.ticket_at + 0xA4) {
            if (out[m.ticket_at + i] != m.ticket[i]) {
                CHECK(!"the ticket changed outside its title id");
                break;
            }
        }
    }
    // the TMD
    const u64 tmd = m.tmd_at, b = tmd + 4 + 0x13C, info = b + 0xC4, chunk = info + 0x900;
    CHECK(B64(out, b + 0x4C) == am);
    CHECK(B16(out, chunk + 6) == 0);  // no title key
    CHECK(B64(out, chunk + 8) == m.plain.size());
    CHECK(ShaIs(out, info, 0x900, out, b + 0xA4));
    CHECK(ShaIs(out, chunk, 0x30, out, info + 4));
    CHECK(ShaIs(out, m.content_at, m.plain.size(), out, chunk + 0x10));
    CHECK(B16(out, b + 0x9C) == B16(m.cia, b + 0x9C));
    // the content: NCCH in the clear
    Bytes c(out.begin() + static_cast<std::ptrdiff_t>(m.content_at), out.begin() + static_cast<std::ptrdiff_t>(m.content_at + m.plain.size()));
    CHECK(c[0x18B] == 0);
    CHECK((c[0x18F] & 0x04) != 0 && (c[0x18F] & 0x01) == 0);
    CHECK(L64(c, 0x108) == am);  // the partition id is the program id, as makerom writes it (the build 2e: the output has no layer to count)
    CHECK_EQ(c[0x20D], m.plain[0x20D] | 0x02);  // the SCI's flags say SDApplication
    const bool desc_sd = (L32(m.plain, 0x9F0) & 0x100) != 0;
    CHECK_EQ(L32(c, 0x5F0), desc_sd ? (L32(m.plain, 0x5F0) | 0x100) : L32(m.plain, 0x5F0));  // the ACI's ARM9 says SdApplication only if its descriptor lets it
    CHECK(L64(c, 0x118) == am && L64(c, 0x3C8) == prepared && L64(c, 0x400) == prepared && L64(c, 0x800) == prepared);  // (2f: 0x400 never AM's)
    CHECK(ShaIs(c, 0x200, 0x400, c, 0x160));
    CHECK(std::memcmp(c.data(), m.plain.data(), 0x100) == 0);  // the signature (the layer's KeyY) kept
    // every other byte is the generator's plain one
    std::vector<std::pair<u64, u64>> changed = {{0x108, 8}, {0x118, 8}, {0x3C8, 8}, {0x400, 8}, {0x800, 8}, {0x20D, 1}, {0x5F0, 4}, {0x160, 32}, {0x18B, 1}, {0x18F, 1}};
    if (hidden) {
        changed.push_back({m.icon_at + 0x2028, 4});
        changed.push_back({m.icon_hash_at, 32});
        changed.push_back({0x1C0, 32});
    }
    u64 other = 0;
    for (u64 i = 0; i < c.size(); ++i) {
        bool in = false;
        for (const auto& r : changed) {
            in = in || (i >= r.first && i < r.first + r.second);
        }
        other += (!in && c[i] != m.plain[i]) ? 1 : 0;
    }
    CHECK_EQ(other, 0);
    const u32 flags = L32(c, m.icon_at + 0x2028);
    CHECK_EQ(flags, hidden ? (L32(m.plain, m.icon_at + 0x2028) & ~1u) : L32(m.plain, m.icon_at + 0x2028));
    CHECK(ShaIs(c, m.icon_at, 0x36C0, c, m.icon_hash_at));
    CHECK(ShaIs(c, m.exefs_at, 0x200, c, 0x1C0));
    CHECK(ShaIs(c, m.romfs_at, 0x200, c, 0x1E0));
    // the paddings and the meta block
    for (u64 i = m.content_at + m.plain.size(); i < m.meta_at; ++i) {
        if (out[i] != 0) {
            CHECK(!"the padding after the content is not zeros");
            break;
        }
    }
    Bytes meta(out.begin() + static_cast<std::ptrdiff_t>(m.meta_at), out.end());
    CHECK_EQ(L32(meta, 0x400 + 0x2028), hidden ? (L32(m.meta, 0x400 + 0x2028) & ~1u) : L32(m.meta, 0x400 + 0x2028));
    meta[0x400 + 0x2028] = m.meta[0x400 + 0x2028];
    CHECK(meta == m.meta);
}

// ---- 6b. the two ids of the build 2f (2026-11-19): the hidden form M3c1 -----------------------------------------------------------------
static void TestTwoIds() {
    std::printf("6b: the two ids (the build 2f): AM's in the ticket, the TMD, 0x108 and 0x118; the prepared one in the exheader\n");
    const synth::Made m = synth::Make(synth::Opts());
    const Keys keys = KeysOf(m.keys);
    const u64 prepared = 0x000400000F8F8200ull, am = 0x000400010F8F8200ull;
    // HeaderPatches: 0x108 and 0x118 get AM's id, 0x3C8, 0x400 and 0x800 the prepared one - never AM's
    PatchList list;
    CHECK(HeaderPatches(m.plain.data(), prepared, am, &list) == NcchWhy::Ok && !list.lost);
    int seen = 0;
    for (u8 i = 0; i < list.n; ++i) {
        const u64 at = list.p[i].at;
        if (at == 0x108 || at == 0x118 || at == 0x3C8 || at == 0x400 || at == 0x800) {
            ++seen;
            CHECK(list.p[i].len == 8 && LoadLe64(list.p[i].bytes) == ((at == 0x108 || at == 0x118) ? am : prepared));
        }
    }
    CHECK_EQ(seen, 5);
    for (int hidden = 0; hidden < 2; ++hidden) {
        const u8 variant = hidden == 1 ? kHide : kVisible;
        Run old_form = Compose1(m.cia, 0, m.cia.size(), keys, prepared, variant, 4096);
        Run same = Compose1(m.cia, 0, m.cia.size(), keys, prepared, variant, 4096, nullptr, nullptr, nullptr, prepared);
        Run two = Compose1(m.cia, 0, m.cia.size(), keys, prepared, variant, 4096, nullptr, nullptr, nullptr, am);
        CHECK(old_form.compose == 0 && same.compose == 0 && two.compose == 0 && old_form.emit == 0 && two.emit == 0);
        CHECK(same.sink.bytes == old_form.sink.bytes);  // (am_title = the prepared id: byte for byte the old form, the fallback's CIA)
        CheckOutput(m, two.sink.bytes, prepared, hidden == 1, am);
        // the exheader is the same in the two forms: its hash at 0x160 too (0x108 and 0x118 are outside it)
        const u64 c = m.content_at;
        CHECK(two.sink.bytes.size() == old_form.sink.bytes.size() &&
              std::memcmp(two.sink.bytes.data() + c + 0x160, old_form.sink.bytes.data() + c + 0x160, 32) == 0 &&
              std::memcmp(two.sink.bytes.data() + c + 0x200, old_form.sink.bytes.data() + c + 0x200, 0x800) == 0);
        // what differs: the ticket's and the TMD's ids, 0x108, 0x118, and the hashes of the content that follow from them - nothing else
        u64 differ = 0;
        for (std::size_t i = 0; i < two.sink.bytes.size() && i < old_form.sink.bytes.size(); ++i) {
            differ += two.sink.bytes[i] != old_form.sink.bytes[i] ? 1 : 0;
        }
        CHECK(differ > 0 && differ <= 2 + 2 + 2 + 2 + 32 * 3);
    }
}

// (the build 2h) the system form: ONE id - the process's (the exheader's) and AM's (the ticket, the TMD, 0x108, 0x118) are 000400100F8F8200, what
// dlp_guest_run.cpp gives the composer (ProcessIdFor and AmTitleFor of kFormSystem). The same bytes as the old form made with that one id.
static void TestSystemForm() {
    std::printf("6c: the system form (the build 2h): 00040010 in the ticket, the TMD, 0x108, 0x118, 0x3C8, 0x400 and 0x800\n");
    const synth::Made m = synth::Make(synth::Opts());
    const Keys keys = KeysOf(m.keys);
    const u64 sys = 0x000400100F8F8200ull;
    PatchList list;
    CHECK(HeaderPatches(m.plain.data(), sys, sys, &list) == NcchWhy::Ok && !list.lost);
    int seen = 0;
    for (u8 i = 0; i < list.n; ++i) {
        const u64 at = list.p[i].at;
        if (at == 0x108 || at == 0x118 || at == 0x3C8 || at == 0x400 || at == 0x800) {
            ++seen;
            CHECK(list.p[i].len == 8 && LoadLe64(list.p[i].bytes) == sys);
        }
    }
    CHECK_EQ(seen, 5);
    Run one = Compose1(m.cia, 0, m.cia.size(), keys, sys, kHide, 4096, nullptr, nullptr, nullptr, sys);
    Run zero = Compose1(m.cia, 0, m.cia.size(), keys, sys, kHide, 4096); // (am_title 0 = the process's id: the same bytes)
    CHECK(one.compose == 0 && one.emit == 0 && zero.compose == 0 && one.sink.bytes == zero.sink.bytes);
    CheckOutput(m, one.sink.bytes, sys, true, sys);
    CiaLayout lay;
    TmdFacts tmd;
    TicketFacts tik;
    CHECK(ReadLayout(one.sink.bytes.data(), one.sink.bytes.size(), &lay) && ReadTmd(one.sink.bytes.data() + lay.tmd_at, lay.tmd, &tmd) &&
          ReadTicket(one.sink.bytes.data() + lay.ticket_at, lay.ticket, &tik));
    const u8* n = one.sink.bytes.data() + lay.content_at;
    CHECK(tmd.tid == sys && tik.tid == sys && LoadLe64(n + 0x108) == sys && LoadLe64(n + 0x118) == sys && LoadLe64(n + 0x3C8) == sys &&
          LoadLe64(n + 0x400) == sys && LoadLe64(n + 0x800) == sys);
    // Luma's loader gives a plugin to this category: (00040010 & 0xFFFFFFED) == 00040000 (loader.c:456)
    CHECK(((static_cast<u32>(LoadLe64(n + 0x400) >> 32)) & 0xFFFFFFEDu) == 0x00040000u);
}

// ---- 6-7. the whole chain on made-up children ---------------------------------------------------------------------------------------

static void TestChain() {
    std::printf("6: the whole chain on made-up children (chunks of 16 B, 4 KB + 16, 256 KB; found in a RomFS)\n");
    const u64 prepared = 0x000400000F8F8200ull;
    const synth::Made m = synth::Make(synth::Opts());
    const Keys keys = KeysOf(m.keys);
    u64 file_at = 0;
    const Bytes image = synth::InRomFs(m.cia, "00040001000f8200.cia", &file_at);
    // where the console finds it: FindChildPackage, then the CIA from its meta block's SMDH
    Mem mem{&image};
    azl::dlp_real::ChildPackage pkg;
    CHECK(azl::dlp_real::FindChildPackage(&Mem::Read, &mem, 0x000F8200u, &pkg) == azl::dlp_real::PackageWhy::Ok);
    const u64 cia_at = pkg.smdh_at - pkg.transfer_size - kMetaSmdhAt;
    CHECK_EQ(cia_at, file_at);
    CHECK_EQ(pkg.required_size, m.cia.size());
    for (std::size_t chunk : {std::size_t(16), std::size_t(4096 + 16), std::size_t(256 * 1024)}) {
        for (u8 variant : {static_cast<u8>(kHide), static_cast<u8>(kVisible)}) {
            Run r = Compose1(image, cia_at, pkg.required_size, keys, prepared, variant, chunk);
            CHECK_EQ(r.compose, 0);
            CHECK_EQ(r.emit, 0);
            CHECK(r.temp.in_order && r.sink.in_order);
            CHECK_EQ(r.temp.written, m.plain.size());
            CheckOutput(m, r.sink.bytes, prepared, variant == kHide);
        }
    }
    CHECK(kDefaultVariant == kHide);

    // a container that is NoCrypto already, a content with no title key: the same output, the steps skipped
    {
        synth::Opts o;
        o.ncch_layer = false;
        o.title_key = false;
        o.rng = 2;
        const synth::Made p = synth::Make(o);
        Run r = Compose1(p.cia, 0, p.cia.size(), KeysOf(p.keys), prepared, kHide, 4096);
        CHECK_EQ(r.compose, 0);
        CheckOutput(p, r.sink.bytes, prepared, true);
    }
    // (the build 2e) a child whose access descriptor does not let the ACI say SdApplication: the ACI's ARM9 stays as it is (an ACI that asks more than its
    // descriptor is refused when the title starts), the rest of the form makerom is made
    {
        synth::Opts o;
        o.desc_sd = false;
        o.rng = 4;
        const synth::Made p = synth::Make(o);
        Run r = Compose1(p.cia, 0, p.cia.size(), KeysOf(p.keys), prepared, kHide, 4096);
        CHECK_EQ(r.compose, 0);
        CheckOutput(p, r.sink.bytes, prepared, true);
        const u64 at = p.content_at;
        CHECK(r.sink.bytes.size() == p.cia.size() && (L32(r.sink.bytes, at + 0x5F0) & 0x100) == 0 && (r.sink.bytes[at + 0x20D] & 0x02) != 0);
    }
    // (the review of the build 2e) a child in the form already, SDApplication in its SCI and SdApplication in its ACI: the |= leaves both as they are
    {
        synth::Opts o;
        o.sci_sd = true;
        o.aci_sd = true;
        o.rng = 5;
        const synth::Made p = synth::Make(o);
        Run r = Compose1(p.cia, 0, p.cia.size(), KeysOf(p.keys), prepared, kHide, 4096);
        CHECK_EQ(r.compose, 0);
        CheckOutput(p, r.sink.bytes, prepared, true);
        const u64 at = p.content_at;
        CHECK(r.sink.bytes.size() == p.cia.size() && r.sink.bytes[at + 0x20D] == p.plain[0x20D] && L32(r.sink.bytes, at + 0x5F0) == L32(p.plain, 0x5F0));  // (the form already: no bit moves)
    }
    // ... an ACI that says SdApplication while its descriptor does not: left as the child has it (the composer never takes a bit away)
    {
        synth::Opts o;
        o.aci_sd = true;   // (above its descriptor)
        o.desc_sd = false; // (the descriptor without the bit)
        o.rng = 6;
        const synth::Made p = synth::Make(o);
        Run r = Compose1(p.cia, 0, p.cia.size(), KeysOf(p.keys), prepared, kHide, 4096);
        CHECK_EQ(r.compose, 0);
        CheckOutput(p, r.sink.bytes, prepared, true);
        const u64 at = p.content_at;
        CHECK(r.sink.bytes.size() == p.cia.size() && (L32(r.sink.bytes, at + 0x5F0) & 0x100) != 0 && L32(r.sink.bytes, at + 0x5F0) == L32(p.plain, 0x5F0));  // (an ACI above its descriptor kept)
    }
    // ... a change the full PatchList refuses is never lost in silence: HeaderPatches (10 changes) and HidePatches (3) say Patches
    {
        const u8 one = 0;
        PatchList room;
        for (u64 i = 0; i < 6; ++i) {
            CHECK(room.Add(0x100000 + i, &one, 1));
        }
        CHECK(HeaderPatches(m.plain.data(), prepared, prepared, &room) == NcchWhy::Ok && room.n == 16 && !room.lost);  // (6 + 10: the list full, none lost)
        PatchList full;
        for (u64 i = 0; i < 7; ++i) {
            CHECK(full.Add(0x100000 + i, &one, 1));
        }
        CHECK(HeaderPatches(m.plain.data(), prepared, prepared, &full) == NcchWhy::Patches && full.lost && full.n == 16);  // (7 + 10: one refused, said)
        PatchList bad;
        CHECK(!bad.Add(0x100000, &one, 0) && bad.lost && bad.n == 0);  // (a length out of 1..32: refused, said)
        const IconPlace place{m.icon_at, m.icon_hash_at};
        for (u64 extra : {13ull, 14ull}) {
            PatchList h;
            for (u64 i = 0; i < extra; ++i) {
                h.Add(0x100000 + i, &one, 1);
            }
            u32 before = 0;
            const NcchWhy why = HidePatches(m.plain.data(), m.exefs_at, m.plain.data() + m.exefs_at, 0x200, place, m.plain.data() + m.icon_at, &h, &before);
            CHECK(why == (extra == 13 ? NcchWhy::Ok : NcchWhy::Patches));  // (13 + 3 fit, 14 + 3 do not)
        }
    }
    // an icon that is hidden already: still a good child, the bit stays off
    {
        synth::Opts o;
        o.icon_flags = 0x144;
        o.rng = 3;
        const synth::Made p = synth::Make(o);
        Run r = Compose1(p.cia, 0, p.cia.size(), KeysOf(p.keys), prepared, kHide, 4096);
        CHECK_EQ(r.compose, 0);
        CheckOutput(p, r.sink.bytes, prepared, true);
    }
    // the steps: told in order, and a cancel in the stream stops it
    {
        u64 at = 0x4000;
        Run r = Compose1(m.cia, 0, m.cia.size(), keys, prepared, kHide, 4096, &CancelAt, &at);
        CHECK_EQ(r.compose, kErrCancel);
        CHECK_EQ(r.sink.calls, 0);
        CHECK(r.temp.written <= 0x4000);
    }

    std::printf("7: a damaged content, a wrong KeyY\n");
    {
        Bytes bad = m.cia;
        bad[m.content_at + m.romfs_at + 0x300] ^= 0x01;  // (a byte of the RomFS: only the TMD's hash can tell)
        Run r = Compose1(bad, 0, bad.size(), keys, prepared, kHide, 4096);
        CHECK_EQ(r.compose, kErrSourceHash);
        CHECK_EQ(r.sink.calls, 0);  // nothing reaches the AM sink
    }
    {
        Keys wrong = keys;
        wrong.common_y0[3] ^= 0x40;
        Run r = Compose1(m.cia, 0, m.cia.size(), wrong, prepared, kHide, 4096);
        CHECK_EQ(r.compose, kErrTitleKey);
        CHECK_EQ(r.temp.writes, 0);  // known before the stream: no byte of the work file
        CHECK_EQ(r.sink.calls, 0);
    }
    {
        Keys wrong = keys;
        wrong.ncch_x[0] ^= 0x80;  // the NCCH layer's key: the exheader's hash says it
        Run r = Compose1(m.cia, 0, m.cia.size(), wrong, prepared, kHide, 4096);
        CHECK_EQ(r.compose, kErrNcch);
        CHECK_EQ(r.temp.writes, 0);
    }
    {
        Keys wrong = keys;
        wrong.generator[15] ^= 0x01;
        Run r = Compose1(m.cia, 0, m.cia.size(), wrong, prepared, kHide, 4096);
        CHECK(r.compose == kErrTitleKey || r.compose == kErrNcch);
        CHECK_EQ(r.temp.writes, 0);
    }
}

// ---- 5. the refusals ---------------------------------------------------------------------------------------------------------------

static int ComposeOf(const synth::Opts& o, const Keys* keys_over = nullptr, u8 variant = kHide) {
    const synth::Made p = synth::Make(o);
    const Keys k = keys_over ? *keys_over : KeysOf(p.keys);
    Run r = Compose1(p.cia, 0, p.cia.size(), k, 0x000400000F8F8200ull, variant, 4096);
    CHECK(r.compose == 0 || r.sink.calls == 0);
    return r.compose;
}

static void TestRefusals() {
    std::printf("5: the refusals\n");
    const synth::Made m = synth::Make(synth::Opts());
    const Keys keys = KeysOf(m.keys);
    const u64 prepared = 0x000400000F8F8200ull;
    // the layout
    CHECK_EQ(Compose1(m.cia, 0, m.cia.size() - 1, keys, prepared, kHide, 4096).compose, kErrNotCia);  // the file is shorter than its parts
    {
        Bytes cut(m.cia.begin(), m.cia.end() - 0x100);
        CHECK_EQ(Compose1(cut, 0, m.cia.size(), keys, prepared, kHide, 4096).compose, kErrNoSource);  // the reads end before the file says
    }
    {
        Bytes bad = m.cia;
        synth::Le32(bad, 0x14, 0x3AC0 + 0x40);  // the meta block: the parts no longer add up to the file
        CHECK_EQ(Compose1(bad, 0, bad.size(), keys, prepared, kHide, 4096).compose, kErrNotCia);
        bad = m.cia;
        synth::Le32(bad, 0, 0x2000);  // the header's size
        CHECK_EQ(Compose1(bad, 0, bad.size(), keys, prepared, kHide, 4096).compose, kErrNotCia);
        bad = m.cia;
        bad[m.tmd_at + 3] = 0x09;  // a type of signature nobody has
        CHECK_EQ(Compose1(bad, 0, bad.size(), keys, prepared, kHide, 4096).compose, kErrNotCia);
        bad = m.cia;
        bad[m.ticket_at + 3] = 0x77;
        CHECK_EQ(Compose1(bad, 0, bad.size(), keys, prepared, kHide, 4096).compose, kErrNotCia);
        bad = m.cia;
        bad[m.tmd_at + 4 + 0x13C + 0x4C + 7] ^= 1;  // the TMD's title id is not the ticket's
        CHECK_EQ(Compose1(bad, 0, bad.size(), keys, prepared, kHide, 4096).compose, kErrNotCia);
        bad = m.cia;
        bad[m.tmd_at + 4 + 0x13C + 0xC4 + 0x900 + 0x14] ^= 1;  // the content record changed under its hash
        CHECK_EQ(Compose1(bad, 0, bad.size(), keys, prepared, kHide, 4096).compose, kErrNotCia);
    }
    synth::Opts o;
    o.contents = 2;
    CHECK_EQ(ComposeOf(o), kErrNotCia);
    o = synth::Opts();
    o.info_count = 2;
    CHECK_EQ(ComposeOf(o), kErrNotCia);
    o = synth::Opts();
    o.child = 0x00040000000F8200ull;  // not a Download Play child
    CHECK_EQ(ComposeOf(o), kErrNotCia);
    o = synth::Opts();
    o.key_index = 1;
    CHECK_EQ(ComposeOf(o), kErrCommonKey);
    o = synth::Opts();
    o.tik_sig = 0x10000;  // RSA-4096: another body, read right
    o.tmd_sig = 0x10005;
    CHECK_EQ(ComposeOf(o), 0);
    // the NCCH
    o = synth::Opts();
    o.method = 1;
    CHECK_EQ(ComposeOf(o), kErrChildCrypto);
    o = synth::Opts();
    o.seed = true;
    CHECK_EQ(ComposeOf(o), kErrChildCrypto);
    o = synth::Opts();
    o.fixed_key = true;
    CHECK_EQ(ComposeOf(o), kErrChildCrypto);
    o = synth::Opts();
    o.exheader_size = 0x200;
    CHECK_EQ(ComposeOf(o), kErrNcch);
    // hidden: no icon, an icon whose hash does not hold
    o = synth::Opts();
    o.icon = false;
    CHECK_EQ(ComposeOf(o), kErrCannotHide);
    CHECK_EQ(ComposeOf(o, nullptr, kVisible), 0);  // (the same child visible: the tests' only use)
    {
        // a byte of the icon damaged under both layers: its hash no longer holds, the child is not hidden (and never installed visible)
        Bytes bad = m.cia;
        bad[m.content_at + m.icon_at + 0x2100] ^= 0x10;  // (cipher bytes: the CBC block and the next change, the icon's hash fails, the TMD's too)
        Run r = Compose1(bad, 0, bad.size(), keys, prepared, kHide, 4096);
        CHECK_EQ(r.compose, kErrCannotHide);
        CHECK_EQ(r.temp.writes, 0);
    }
    CHECK_EQ(ComposeOf(synth::Opts(), nullptr, kHide | kFixCrr), kErrVariant);
    // bad buffers
    {
        Mem src{&m.cia};
        ComposeIn in;
        in.read = &Mem::Read;
        in.rctx = &src;
        in.cia_size = m.cia.size();
        in.keys = &keys;
        in.prepared = prepared;
        Temp t;
        in.temp = &Temp::Write;
        in.tctx = &t;
        std::vector<u8> buf(4096 + 8), work(kWorkBytes - 1);
        in.buf = buf.data();
        in.buf_size = buf.size();  // not a multiple of 16
        in.work = work.data();
        in.work_size = kWorkBytes;
        Composed* c = new Composed;
        CHECK_EQ(Compose(in, c), kErrNoMemory);
        in.buf_size = 4096;
        in.work_size = work.size();  // too small
        CHECK_EQ(Compose(in, c), kErrNoMemory);
        delete c;
    }
}

// ---- 12. method 1: the KeyX of slot 0x25 in boot.firm (the build 2e slot25) - made-up keys and a made-up boot.firm only ----------------------

static std::string Lower(std::string s) {
    for (char& ch : s) {
        ch = (ch >= 'A' && ch <= 'F') ? static_cast<char>(ch - 'A' + 'a') : ch;
    }
    return s;
}

static void TestSlot25() {
    std::printf("12: method 1, the KeyX of slot 0x25 in a made-up boot.firm (made-up keys only)\n");
    const u64 prepared = 0x000400000F8F8200ull;
    synth::Opts o;
    o.method = 1;
    o.banner = true;
    o.logo = true;
    o.rng = 11;
    const synth::Made m = synth::Make(o);
    const Keys keys = KeysOf(m.keys);
    const synth::Firm firm = synth::MakeFirm(m.keys.slot25_x, synth::FirmOpts());
    const std::string key_hex = Lower(ToHex(m.keys.slot25_x, 16));
    // found in the ARM9's section, at every chunk, hidden and visible: the same plain child a child of method 0 gives (CheckOutput), the KeyX's offset
    // said and nothing of it; boot.firm asked for once
    for (std::size_t chunk : {std::size_t(16), std::size_t(4096 + 16), std::size_t(256 * 1024)}) {
        for (u8 variant : {static_cast<u8>(kHide), static_cast<u8>(kVisible)}) {
            Run r = Compose1(m.cia, 0, m.cia.size(), keys, prepared, variant, chunk, nullptr, nullptr, &firm.bytes);
            CHECK_EQ(r.compose, 0);
            CHECK_EQ(r.emit, 0);
            CheckOutput(m, r.sink.bytes, prepared, variant == kHide);
            CHECK(r.slot25 == Slot25Step::Found && r.slot25_at == firm.key_at && r.firm_asks == 1);
            CHECK_EQ(r.candidates, (0x5200 - 0x200) / 4 + 1);  // (the first pass, the ARM9's section, up to the KeyX: the false ones before it refused)
        }
    }
    {
        Run r = Compose1(m.cia, 0, m.cia.size(), keys, prepared, kHide, 4096, nullptr, nullptr, &firm.bytes);
        CHECK(r.line == "guest: child crypto 00040001000F8200 game 00040000000F8200 flags 0000000101030000 slot 0x25 key found at 0x00005200, 5121 candidates, 0 ms");
        CHECK(Lower(r.line).find(key_hex) == std::string::npos);  // the KeyX is never in the line
        CHECK(r.line.size() < kCryptoLineBytes - 1);
    }
    // (the review of DPAPP-2e slot25) the time of the line is the search's alone: boot.firm's read (1000 ms of the made-up clock) is not in it
    {
        g_now = 0;
        g_clock = true;
        Run r = Compose1(m.cia, 0, m.cia.size(), keys, prepared, kHide, 4096, nullptr, nullptr, &firm.bytes);
        g_clock = false;
        CHECK(r.slot25 == Slot25Step::Found);
        CHECK(r.line.find(" key found at 0x00005200, 5121 candidates, 3 ms") != std::string::npos);
    }
    // (the review) the candidates are aligned to 4 where the section is loaded, not in the file: a section loaded 2 bytes off the file's boundaries
    {
        synth::FirmOpts f;
        f.unaligned = true;         // the KeyX at 0x5202 of the file...
        f.arm9_load = 0x08006002u;  // ... at 0x0800B004 in the ARM9's memory: aligned
        const synth::Firm moved = synth::MakeFirm(m.keys.slot25_x, f);
        Run r = Compose1(m.cia, 0, m.cia.size(), keys, prepared, kHide, 4096, nullptr, nullptr, &moved.bytes);
        CHECK_EQ(r.compose, 0);
        CheckOutput(m, r.sink.bytes, prepared, true);
        CHECK(r.slot25 == Slot25Step::Found && r.slot25_at == moved.key_at && moved.key_at == 0x5202);
        CHECK_EQ(r.candidates, (0x5202 - 0x202) / 4 + 1);
    }
    // (the review) a damaged exheader is 309 before boot.firm is asked for: the search runs once the primary key holds the exheader's hash
    {
        Bytes cia = m.cia;
        cia[m.content_at + 0x300] ^= 0x01;
        Run r = Compose1(cia, 0, cia.size(), keys, prepared, kHide, 4096, nullptr, nullptr, &firm.bytes);
        CHECK_EQ(r.compose, kErrNcch);
        CHECK(r.firm_asks == 0 && r.slot25 == Slot25Step::None && r.temp.writes == 0);
    }
    // the KeyX in the ARM11's section only: found by the second pass, after every candidate of the ARM9's
    {
        synth::FirmOpts f;
        f.in_arm9 = false;
        const synth::Firm other = synth::MakeFirm(m.keys.slot25_x, f);
        Run r = Compose1(m.cia, 0, m.cia.size(), keys, prepared, kHide, 4096, nullptr, nullptr, &other.bytes);
        CHECK_EQ(r.compose, 0);
        CheckOutput(m, r.sink.bytes, prepared, true);
        CHECK(r.slot25 == Slot25Step::Found && r.slot25_at == other.key_at);
        CHECK_EQ(r.candidates, ((0x8000 - 16) / 4 + 1) + ((0x8300 - 0x8200) / 4 + 1));
    }
    // not found: no KeyX, or only at an offset not aligned to 4 - every candidate of the three sections tried; known before the stream
    for (int unaligned = 0; unaligned < 2; ++unaligned) {
        synth::FirmOpts f;
        f.key = unaligned == 1;
        f.unaligned = unaligned == 1;
        const synth::Firm none = synth::MakeFirm(m.keys.slot25_x, f);
        Run r = Compose1(m.cia, 0, m.cia.size(), keys, prepared, kHide, 4096, nullptr, nullptr, &none.bytes);
        CHECK_EQ(r.compose, kErrChildCrypto);
        CHECK(r.slot25 == Slot25Step::NoKey && r.slot25_at == 0);
        CHECK_EQ(r.candidates, ((0x8000 - 16) / 4 + 1) + ((0x4000 - 16) / 4 + 1) + ((0x1000 - 16) / 4 + 1));
        CHECK_EQ(r.temp.writes, 0);
        CHECK_EQ(r.sink.calls, 0);
        CHECK(r.line == "guest: child crypto 00040001000F8200 game 00040000000F8200 flags 0000000101030000 why Method (0x18B=01: slot 0x25) step no key, 13303 candidates, 0 ms");
    }
    // no boot.firm: as before the build, 308, with the step
    {
        Run r = Compose1(m.cia, 0, m.cia.size(), keys, prepared, kHide, 4096);
        CHECK_EQ(r.compose, kErrChildCrypto);
        CHECK(r.slot25 == Slot25Step::FirmMissing && r.temp.writes == 0 && r.sink.calls == 0);
        CHECK(r.line == "guest: child crypto 00040001000F8200 game 00040000000F8200 flags 0000000101030000 why Method (0x18B=01: slot 0x25) step firm missing, 0 candidates, 0 ms");
        FirmSource gone{nullptr};
        const u8* data = nullptr;
        std::size_t size = 0;
        CHECK(!FirmSource::Get(&gone, &data, &size));
    }
    {
        Run r = Compose1(m.cia, 0, m.cia.size(), keys, prepared, kHide, 4096, nullptr, nullptr, nullptr);  // (no FirmFn at all)
        CHECK(r.slot25 == Slot25Step::FirmMissing && r.firm_asks == 0);
    }
    // a boot.firm that is not one to trust: a section's SHA-256 wrong, no "FIRM", a file too short or too big - no candidate is tried
    {
        synth::FirmOpts f;
        f.bad_hash = true;
        const synth::Firm bad = synth::MakeFirm(m.keys.slot25_x, f);
        f = synth::FirmOpts();
        f.magic = false;
        const synth::Firm nomagic = synth::MakeFirm(m.keys.slot25_x, f);
        Bytes small(firm.bytes.begin(), firm.bytes.begin() + 0x100);
        Bytes big = firm.bytes;
        big.resize(kFirmMaxBytes + 1, 0);
        Bytes cut(firm.bytes.begin(), firm.bytes.end() - 0x10);  // (the last section outside the file)
        for (const Bytes* b : std::vector<const Bytes*>{&bad.bytes, &nomagic.bytes, &small, &big, &cut}) {
            Run r = Compose1(m.cia, 0, m.cia.size(), keys, prepared, kHide, 4096, nullptr, nullptr, b);
            CHECK_EQ(r.compose, kErrChildCrypto);
            CHECK(r.slot25 == Slot25Step::FirmBad && r.candidates == 0 && r.temp.writes == 0);
        }
        Run r = Compose1(m.cia, 0, m.cia.size(), keys, prepared, kHide, 4096, nullptr, nullptr, &bad.bytes);
        CHECK(r.line == "guest: child crypto 00040001000F8200 game 00040000000F8200 flags 0000000101030000 why Method (0x18B=01: slot 0x25) step firm bad, 0 candidates, 0 ms");
    }
    // no RomFS: nothing to know the key by - boot.firm is not even asked for
    {
        synth::Opts n = o;
        n.romfs_units = 0;
        const synth::Made p = synth::Make(n);
        Run r = Compose1(p.cia, 0, p.cia.size(), KeysOf(p.keys), prepared, kHide, 4096, nullptr, nullptr, &firm.bytes);
        CHECK_EQ(r.compose, kErrChildCrypto);
        CHECK(r.slot25 == Slot25Step::NoRomfs && r.firm_asks == 0 && r.temp.writes == 0);
    }
    // (the review) a superblock larger than the room to check it: its own step, no candidate tried, boot.firm not asked for
    {
        synth::Opts n = o;
        n.romfs_hashed_units = 0x1C;  // (0x3800 bytes: more than kSmdhSize)
        const synth::Made p = synth::Make(n);
        Run r = Compose1(p.cia, 0, p.cia.size(), KeysOf(p.keys), prepared, kHide, 4096, nullptr, nullptr, &firm.bytes);
        CHECK_EQ(r.compose, kErrChildCrypto);
        CHECK(r.slot25 == Slot25Step::RomfsLarge && r.firm_asks == 0 && r.candidates == 0);
        CHECK(r.line.find(" step romfs too large, 0 candidates, 0 ms") != std::string::npos);
    }
    // 0x1E0 wrong: the candidate gives "IVFC" but not the superblock's hash - refused, its offset said (the first confirmation)
    {
        synth::Opts n = o;
        n.bad_romfs_hash = true;
        const synth::Made p = synth::Make(n);
        const synth::Firm pf = synth::MakeFirm(p.keys.slot25_x, synth::FirmOpts());
        Run r = Compose1(p.cia, 0, p.cia.size(), KeysOf(p.keys), prepared, kHide, 4096, nullptr, nullptr, &pf.bytes);
        CHECK_EQ(r.compose, kErrChildCrypto);
        CHECK(r.slot25 == Slot25Step::RomfsHash && r.slot25_at == pf.key_at && r.temp.writes == 0);
        CHECK(r.line.find(" step romfs hash at 0x00005200, 13303 candidates, 0 ms") != std::string::npos);
    }
    // .code's hash wrong: the key found, the stream made, .code does not hold - nothing reaches the AM sink (the second confirmation)
    {
        synth::Opts n = o;
        n.bad_code_hash = true;
        const synth::Made p = synth::Make(n);
        const synth::Firm pf = synth::MakeFirm(p.keys.slot25_x, synth::FirmOpts());
        Run r = Compose1(p.cia, 0, p.cia.size(), KeysOf(p.keys), prepared, kHide, 4096, nullptr, nullptr, &pf.bytes);
        CHECK_EQ(r.compose, kErrChildCrypto);
        CHECK(r.slot25 == Slot25Step::CodeHash && r.slot25_at == pf.key_at && r.sink.calls == 0);
        CHECK(r.line.find(" step code hash at 0x00005200, 5121 candidates") != std::string::npos);
    }
    // a boot.firm whose only "KeyX" is the one of slot 0x2C (the primary key's): it gives no "IVFC" - method 1's RomFS is under the other key
    {
        const synth::Firm wrong = synth::MakeFirm(m.keys.ncch_x, synth::FirmOpts());
        Run r = Compose1(m.cia, 0, m.cia.size(), keys, prepared, kHide, 4096, nullptr, nullptr, &wrong.bytes);
        CHECK_EQ(r.compose, kErrChildCrypto);
        CHECK(r.slot25 == Slot25Step::NoKey);
    }
    // a child of method 0 never asks for boot.firm, and its output does not move (CheckOutput above, TestChain); method 1 with NoCrypto neither
    {
        const synth::Made z = synth::Make(synth::Opts());
        Run r = Compose1(z.cia, 0, z.cia.size(), KeysOf(z.keys), prepared, kHide, 4096, nullptr, nullptr, &firm.bytes);
        CHECK_EQ(r.compose, 0);
        CHECK(r.firm_asks == 0 && r.slot25 == Slot25Step::None);
        synth::Opts n = o;
        n.ncch_layer = false;
        const synth::Made p = synth::Make(n);
        Run q = Compose1(p.cia, 0, p.cia.size(), KeysOf(p.keys), prepared, kHide, 4096, nullptr, nullptr, &firm.bytes);
        CHECK_EQ(q.compose, 0);
        CHECK(q.firm_asks == 0 && q.slot25 == Slot25Step::None);
    }
    // the other cases of 308, said by the line: 0x0A, 0x0B, a value nobody knows, seed, fixed key
    {
        const struct {
            u8 method;
            bool seed, fixed;
            const char* tail;
        } cases[] = {{0x0A, false, false, "flags 0000000A01030000 why Method (0x18B=0A: slot 0x18, not supported)"},
                     {0x0B, false, false, "flags 0000000B01030000 why Method (0x18B=0B: slot 0x1B, not supported)"},
                     {0x05, false, false, "flags 0000000501030000 why Method (0x18B=05: unknown)"},
                     {0x00, true, false, "flags 0000000001030020 why Seed (0x18F=20)"},
                     {0x00, false, true, "flags 0000000001030001 why FixedKey (0x18F=01)"}};
        for (const auto& k : cases) {
            synth::Opts n;
            n.method = k.method;
            n.seed = k.seed;
            n.fixed_key = k.fixed;
            const synth::Made p = synth::Make(n);
            Run r = Compose1(p.cia, 0, p.cia.size(), KeysOf(p.keys), prepared, kHide, 4096, nullptr, nullptr, &firm.bytes);
            CHECK_EQ(r.compose, kErrChildCrypto);
            CHECK(r.firm_asks == 0 && r.slot25 == Slot25Step::None);
            CHECK(r.line == std::string("guest: child crypto 00040001000F8200 game 00040000000F8200 ") + k.tail);
        }
    }
    // ExefsKeys: which file under which key, and the ExeFS's that are refused
    {
        NcchPlan plan{};
        plan.exefs_at = 0xA00;
        plan.exefs_size = 0x200 + 0x4000;
        u8 h[0x200] = {};
        const auto entry = [&h](int i, const char* name, u32 off, u32 size) {
            std::memcpy(h + 16 * i, name, std::strlen(name));
            StoreLe32(h + 16 * i + 8, off);
            StoreLe32(h + 16 * i + 12, size);
        };
        entry(0, ".code", 0, 0x1001);
        entry(1, "icon", 0x1200, 0x1E00);
        entry(2, "banner", 0x3000, 0x400);
        entry(3, "logo", 0x3400, 0x10);
        h[0xC0 + 9 * 32] = 0x77;
        CHECK(ExefsKeys(h, &plan) == NcchWhy::Ok);
        CHECK_EQ(plan.n_secondary, 2);  // .code and logo; icon and banner under the primary key
        CHECK(plan.secondary[0].at == 0xC00 && plan.secondary[0].size == 0x1200);  // (.code to its end rounded up to 0x200)
        CHECK(plan.secondary[1].at == 0xC00 + 0x3400 && plan.secondary[1].size == 0x200);
        CHECK(plan.code_at == 0xC00 && plan.code_size == 0x1001 && plan.code_hash[0] == 0x77);
        u8 bad[0x200];
        std::memcpy(bad, h, sizeof(bad));
        StoreLe32(bad + 3 * 16 + 8, 0x3408);  // (logo not on 16 bytes)
        CHECK(ExefsKeys(bad, &plan) == NcchWhy::Regions);
        std::memcpy(bad, h, sizeof(bad));
        StoreLe32(bad + 3 * 16 + 8, 0x1000);  // (logo over .code's rounded end)
        CHECK(ExefsKeys(bad, &plan) == NcchWhy::Regions);
        std::memcpy(bad, h, sizeof(bad));
        StoreLe32(bad + 3 * 16 + 12, 0x4000);  // (past the ExeFS)
        CHECK(ExefsKeys(bad, &plan) == NcchWhy::Regions);
        std::memcpy(bad, h, sizeof(bad));
        StoreLe32(bad + 1 * 16 + 8, 0x1010);  // (icon inside .code's span rounded up to 0x200, not inside .code: the review of DPAPP-2e slot25)
        StoreLe32(bad + 1 * 16 + 12, 0x100);
        CHECK(ExefsKeys(bad, &plan) == NcchWhy::Regions);
        std::memcpy(bad, h, sizeof(bad));
        StoreLe32(bad + 2 * 16 + 8, 0x3200);  // (banner under logo's span)
        StoreLe32(bad + 3 * 16 + 8, 0x3100);
        CHECK(ExefsKeys(bad, &plan) == NcchWhy::Regions);
    }
}

// ---- 10. the words --------------------------------------------------------------------------------------------------------------------

static void TestWords() {
    std::printf("10: every result has its words\n");
    const std::string fallback = Phrase(-1);
    for (int e = kErrFirst; e <= kErrLast; ++e) {
        CHECK(std::string(Phrase(e)) != fallback);
        CHECK(std::strlen(Advice(e)) > 0);
        CHECK(std::strlen(Phrase(e)) < 80 && std::strlen(Advice(e)) < 90);
    }
    CHECK(std::string(Phrase(kErrTitleKey)).find("boot9.bin") != std::string::npos);  // (a wrong title key means a wrong boot9.bin: the critic's finding 12)
    CHECK(!CanTryAgain(kErrCannotHide) && !CanTryAgain(kErrTitleKey) && CanTryAgain(kErrNoSpace));
    CHECK(std::string(Advice(kErrTooMany)) == "Remove some guests in Download Play > Enable games.");  // (the build 2e: the page's name now)
    CHECK(std::string(Phrase(kErrChildCrypto)) == "The guest is locked in a way 3DS Link could not open (see the log).");  // (DPAPP-2e slot25 rev)
    CHECK(std::string(Advice(kErrChildCrypto)) == "Start Luma3DS from sd:/boot.firm and try again; if it fails, join with your own copy.");
    // (the final review of 1.1.4) a card's route F: FS's "not found" (lvl 25, sum 4, mod 17, desc 120; any level) is "no child", remembered;
    // every other refusal - another description, summary or module, success - is "no source", tried again (written out, not the code's masks)
    const u32 not_found = (25u << 27) | (4u << 21) | (17u << 10) | 120u;
    CHECK_EQ(not_found, 0xC8804478u);
    CHECK_EQ(CardRouteFRefused(not_found), kErrNoChild);
    CHECK_EQ(CardRouteFRefused((27u << 27) | (4u << 21) | (17u << 10) | 120u), kErrNoChild);
    CHECK_EQ(CardRouteFRefused(not_found + 1), kErrNoSource);             // (desc 121)
    CHECK_EQ(CardRouteFRefused((25u << 27) | (5u << 21) | (17u << 10) | 120u), kErrNoSource);  // (another summary)
    CHECK_EQ(CardRouteFRefused((25u << 27) | (4u << 21) | (16u << 10) | 120u), kErrNoSource);  // (another module)
    CHECK_EQ(CardRouteFRefused(0xD8A0A3F9u), kErrNoSource);
    CHECK_EQ(CardRouteFRefused(0u), kErrNoSource);
}

// ---- 11. the private files of the working tree ----------------------------------------------------------------------------------------

static bool ReadFile(const char* path, Bytes* out) {
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out->assign(n > 0 ? static_cast<std::size_t>(n) : 0, 0);
    const bool ok = n > 0 && std::fread(out->data(), 1, out->size(), f) == out->size();
    std::fclose(f);
    return ok;
}

static void TestRealFiles() {
    std::printf("11: the child of Island Tour against the PC's V2vis, V2, M3c1 and M3a (private files, read where they are)\n");
    const char* kSource = "analysis/island-tour-2026-11-13/figlio/romfs/00040001000f8200.cia";
    const char* kBoot9 = "../keys/boot9.bin";
    // (the build 2f) the third: the hidden form M3c1 - AM's id 000400010F8F8200 (prova-2e-2026-11-19/prova-pc/varianti, made by componi_m3.py from
    // IT-V2 and again by componi_figlio.py from the child's own CIA: the same md5); V2 stays the oracle of the old form, the fallback's
    // (the build 2h) the fourth: the form made now, M3a - ONE id 000400100F8F8200 in the ticket, the TMD, the NCCH and the exheader
    // (prova-2e-2026-11-19/prova-pc/varianti/IT-M3a.cia, made by componi_m3.py from IT-V2; componi_figlio.py of the build 2h makes it again from the
    // child's own CIA with the same md5, and leggi_cia.py, which shares no code with either, reads every id of it as 000400100F8F8200)
    const char* const kOracles[4] = {"analysis/piano-1.1.4-2026-11-15/home-figlio-2026-11-18/varianti/IT-V2vis.cia",
                                     "analysis/piano-1.1.4-2026-11-15/home-figlio-2026-11-18/varianti/IT-V2.cia",
                                     "analysis/piano-1.1.4-2026-11-15/prova-2e-2026-11-19/prova-pc/varianti/IT-M3c1.cia",
                                     "analysis/piano-1.1.4-2026-11-15/prova-2e-2026-11-19/prova-pc/varianti/IT-M3a.cia"};
    const char* const kMd5[4] = {"a3774a9d6e22d4bc221afdfefef78a24", "50482992c22b89727dca37491a6c23c8", "9896d8d132208da5d63fb13ad62aa83f",
                                 "0709e310f52fe9e64c27ba521986f1f8"};
    const char* const kNames[4] = {"V2vis (visible)", "V2 (hidden)    ", "M3c1 (hidden)  ", "M3a (hidden)   "};
    Bytes source, boot9;
    if (!ReadFile(kSource, &source) || !ReadFile(kBoot9, &boot9)) {
        std::printf("   *** TestRealFiles SKIPPED: %s or %s is not here (a published tree has neither): the md5 of V2vis and V2 were NOT checked ***\n",
                    kSource, kBoot9);
        return;
    }
    Keys keys;
    const int keyed = ComposeKeys(boot9.data(), boot9.size(), &keys);
    Wipe(boot9.data(), boot9.size());
    CHECK_EQ(keyed, 0);
    const u64 prepared = azl::title_rules::PreparedOf(0x00040000000F8200ull);
    CHECK_EQ(prepared, 0x000400000F8F8200ull);
    for (int v = 0; v < 4; ++v) {
        const u8 variant = v == 0 ? kVisible : kHide;
        const u64 am = v == 2 ? 0x000400010F8F8200ull : (v == 3 ? 0x000400100F8F8200ull : 0);
        // (the build 2h) the system form: the process's id (the exheader's) is the system id too - what dlp_guest_run.cpp gives the composer
        const u64 process = v == 3 ? azl::title_rules::PreparedSystemOf(prepared) : prepared;
        CHECK(v != 3 || process == am);
        Run r = Compose1(source, 0, source.size(), keys, process, variant, 256 * 1024, nullptr, nullptr, nullptr, am);
        CHECK_EQ(r.compose, 0);
        CHECK_EQ(r.emit, 0);
        const std::string md5 = r.sink.md5.Hex();
        CHECK(md5 == kMd5[v]);
        Bytes oracle;
        if (ReadFile(kOracles[v], &oracle)) {
            u64 differ = oracle.size() == r.sink.bytes.size() ? 0 : 1;
            for (std::size_t i = 0; differ == 0 && i < oracle.size(); ++i) {
                differ += oracle[i] != r.sink.bytes[i] ? 1 : 0;
            }
            CHECK_EQ(differ, 0);
            std::printf("   %s: %zu bytes, md5 %s, %s the PC's %s byte for byte\n", kNames[v], r.sink.bytes.size(), md5.c_str(),
                        differ == 0 ? "the same as" : "NOT the same as", kOracles[v]);
        } else {
            std::printf("   %s: md5 %s (%s), the oracle file is not here to compare byte for byte\n", kNames[v], md5.c_str(),
                        md5 == kMd5[v] ? "the PC's" : "NOT the PC's");
        }
    }
    Wipe(&keys, sizeof(keys));
}

int main() {
    TestAes();
    TestKeys();
    TestIds();
    TestRefusals();
    TestChain();
    TestTwoIds();
    TestSystemForm();
    TestWords();
    TestSlot25();
    TestRealFiles();
    std::printf("dlp_guest_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
