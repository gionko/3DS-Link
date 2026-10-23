// The two values 3DS Link carries (ConfigApp/titleshare_host/builtin_keys.c: the constant of the key generator and the IV of the Download Play checksum),
// run by Tests/run.sh with TitleShare's own aes, sdcrypt and sha256 (the vendored copies). What it pins:
//  * the two names give their 16 bytes and every other name (another case, a prefix, an empty one, none) gives -1 and zeros;
//  * the 16 bytes are the right ones: only their SHA-256 is written here (the old code, with the constant inline, gave the same answers: the three known
//    answers below are the hashes of what ncch_scramble made of three pairs of made-up X and Y with it; a wrong constant cannot give them);
//  * a call changes nothing for the next one, and a hundred calls from four threads at once all give the same bytes.
// The values themselves are not in this file.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "../../ConfigApp/titleshare/common/aes.h"
#include "../../ConfigApp/titleshare/common/sdcrypt.h"
#include "../../ConfigApp/titleshare/common/sha256.h"
#include "../../ConfigApp/titleshare_host/builtin_keys.h"
}

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

static std::string Sha(const uint8_t* data, size_t len) {
    uint8_t out[32];
    sha256(data, len, out);
    char hex[65];
    for (int i = 0; i < 32; ++i) {
        std::snprintf(hex + 2 * i, 3, "%02x", out[i]);
    }
    return std::string(hex);
}

static bool AllZero(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

int main() {
    uint8_t c[16], iv[16];
    CHECK(bk_get(BK_GENERATOR_CONSTANT, c) == 0);
    CHECK(bk_get(BK_DLP_CHECKSUM_IV, iv) == 0);
    CHECK(!AllZero(c, 16) && !AllZero(iv, 16));
    CHECK(std::memcmp(c, iv, 16) != 0);

    // the right bytes: their hashes
    CHECK(Sha(c, 16) == "05d6564396705f79890a12cd05dd914b0adc01ccaa4d5158a90bb32553025997");
    CHECK(Sha(iv, 16) == "c3189f2828a025438eb47194056b1743b99a39ec2045e994f06a8b9e828821e8");

    // any other name: -1 and zeros in the output
    const char* const others[] = {"", "generatorconstant", "GeneratorConstant", "generator", "generatorConstant ", "generatorConstant=", "dlpChecksumModIV", "dlpChecksumMod",
                                  "dlpKeyY", "otpKey", "=", "name"};
    for (const char* name : others) {
        uint8_t out[16];
        std::memset(out, 0xEE, sizeof(out));
        CHECK(bk_get(name, out) == -1);
        CHECK(AllZero(out, 16));
    }
    {
        uint8_t out[16];
        std::memset(out, 0xEE, sizeof(out));
        CHECK(bk_get(nullptr, out) == -1 && AllZero(out, 16));
    }

    // the same answer every time, and a call that fails does not spoil the next one
    for (int i = 0; i < 50; ++i) {
        uint8_t again[16], bad[16];
        CHECK(bk_get("nothing", bad) == -1);
        CHECK(bk_get(BK_GENERATOR_CONSTANT, again) == 0 && std::memcmp(again, c, 16) == 0);
        CHECK(bk_get(BK_DLP_CHECKSUM_IV, again) == 0 && std::memcmp(again, iv, 16) == 0);
    }

    // the key generator: the old code (the constant inline) gave these three answers for these three pairs
    const char* const expected[3] = {"b7e303e9b54e9ce139f2f3c4768afee7d67b82f9e899ed34b3cc0f5f80b423b9", "ff0836faf822a86513e8837d5b03ccc2943e6b2f9ef8b4dad86fa00a77140d95",
                                     "4f5512ab6a9ba95a0fff232ab6cde1a6147d0f0eab9a1e79cd627e0d4a5292e3"};
    for (int set = 0; set < 3; ++set) {
        uint8_t x[16], y[16], key[16];
        for (int i = 0; i < 16; ++i) {
            x[i] = static_cast<uint8_t>(set == 0 ? i : set == 1 ? 0xA5 ^ (i * 7) : 0xFF - i);
            y[i] = static_cast<uint8_t>(set == 0 ? 0x10 + i : set == 1 ? 0x3C + i * 13 : i * 17);
        }
        ncch_scramble(x, y, key);
        CHECK(Sha(key, 16) == expected[set]);
    }

    // four threads, the first call of each at the same moment: the same bytes
    std::vector<std::thread> threads;
    std::vector<int> bad(4, 0);
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&bad, t, &c, &iv]() {
            for (int i = 0; i < 100; ++i) {
                uint8_t a[16], b[16];
                if (bk_get(BK_GENERATOR_CONSTANT, a) != 0 || std::memcmp(a, c, 16) != 0) {
                    ++bad[t];
                }
                if (bk_get(BK_DLP_CHECKSUM_IV, b) != 0 || std::memcmp(b, iv, 16) != 0) {
                    ++bad[t];
                }
            }
        });
    }
    for (std::thread& th : threads) {
        th.join();
    }
    for (int t = 0; t < 4; ++t) {
        CHECK(bad[t] == 0);
    }

    std::printf("builtin_keys_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
