// Tests for the ARM instruction relocator (Sources/azl/arm_reloc.cpp).
//   reloc_test [path-to-decompressed-.code] [text-size]
// Without arguments only the synthetic cases run. With a game's decompressed .code (loaded at
// 0x00100000) it also sweeps every `svc 0x32` / `svc 0x23` site and reports how many can be hooked.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "azl/arm_reloc.hpp"

using namespace azl;

static int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAIL line %d: %s\n", __LINE__, #cond);                        \
        }                                                                                          \
    } while (0)

static u32 ReadFromVector(void* ctx, u32 address) {
    const auto* mem = static_cast<const std::vector<u8>*>(ctx);
    const u32 off = address - 0x00100000;
    if (off + 4 > mem->size()) {
        return 0xDEADBEEF;
    }
    return LoadLE32(mem->data() + off);
}

static u32 ReadNothing(void*, u32) {
    return 0;
}

static void TestSynthetic() {
    std::printf("relocator: synthetic cases\n");
    const u32 kLdrPc = 0xE51FF004;

    // and r1, r0, #0x80000000 : pure data processing, copied verbatim
    RelocResult r = RelocateArm(0xE2001102, 0x1000, 0x1008, ReadNothing, nullptr);
    CHECK(r.ok && r.count == 3 && r.words[0] == 0xE2001102 && r.words[1] == kLdrPc && r.words[2] == 0x1008);
    // ands r1, r0, #0x80000000 (sets flags the next bmi needs) : also copied
    r = RelocateArm(0xE2101102, 0x1000, 0x1008, ReadNothing, nullptr);
    CHECK(r.ok && r.words[0] == 0xE2101102);
    // bx lr (what libctru-style stubs have after the svc)
    r = RelocateArm(0xE12FFF1E, 0x1000, 0x1008, ReadNothing, nullptr);
    CHECK(r.ok && r.words[0] == 0xE12FFF1E);
    // pop {r4, pc}
    r = RelocateArm(0xE8BD8010, 0x1000, 0x1008, ReadNothing, nullptr);
    CHECK(r.ok && r.words[0] == 0xE8BD8010);
    // stmia r5, {r7, r8}
    r = RelocateArm(0xE8850180, 0x1000, 0x1008, ReadNothing, nullptr);
    CHECK(r.ok);
    // ldr r0, [r4, #4]
    r = RelocateArm(0xE5940004, 0x1000, 0x1008, ReadNothing, nullptr);
    CHECK(r.ok && r.words[0] == 0xE5940004);
    // mrc p15, 0, r4, c13, c0, 3 (read TLS)
    r = RelocateArm(0xEE1D4F70, 0x1000, 0x1008, ReadNothing, nullptr);
    CHECK(r.ok);

    // ldr r0, [pc, #0x1C] at 0x2000 -> literal at 0x2000 + 8 + 0x1C = 0x2024; value is copied
    struct Ctx {
        u32 expected_address;
        u32 value;
    } ctx = {0x2024, 0xCAFEBABE};
    auto read = [](void* c, u32 a) -> u32 {
        auto* x = static_cast<Ctx*>(c);
        return a == x->expected_address ? x->value : 0;
    };
    r = RelocateArm(0xE59F001C, 0x2000, 0x2004, read, &ctx);
    CHECK(r.ok && r.count == 4);
    CHECK(r.ok && r.words[0] == 0xE59F0004); // ldr r0, [pc, #4]  -> words[3]
    CHECK(r.ok && r.words[3] == 0xCAFEBABE);
    // conditional literal load keeps its condition
    r = RelocateArm(0x159F001C, 0x2000, 0x2004, read, &ctx); // ldrne
    CHECK(r.ok && (r.words[0] >> 28) == 0x1);

    // add r2, pc, #0x10 at 0x3000 -> r2 = 0x3000 + 8 + 0x10
    r = RelocateArm(0xE28F2010, 0x3000, 0x3004, ReadNothing, nullptr);
    CHECK(r.ok && r.count == 4 && r.words[3] == 0x3018 && r.words[0] == 0xE59F2004);
    // sub r2, pc, #0x10
    r = RelocateArm(0xE24F2010, 0x3000, 0x3004, ReadNothing, nullptr);
    CHECK(r.ok && r.words[3] == 0x2FF8);
    // adds r2, pc, #0x10 needs the flags: refused
    r = RelocateArm(0xE29F2010, 0x3000, 0x3004, ReadNothing, nullptr);
    CHECK(!r.ok);

    // b +0x10 at 0x4000 (imm24 = 4 -> offset 0x10): target = 0x4000 + 8 + 0x10
    r = RelocateArm(0xEA000004, 0x4000, 0x4004, ReadNothing, nullptr);
    CHECK(r.ok && r.words[3] == 0x4018 && r.words[0] == 0xE59FF004);
    // beq backwards: cond preserved, target computed with sign extension
    r = RelocateArm(0x0AFFFFFE, 0x5000, 0x5004, ReadNothing, nullptr); // beq .
    CHECK(r.ok && r.words[3] == 0x5000 && (r.words[0] >> 28) == 0);
    // bl cannot be relocated (lr)
    CHECK(!RelocateArm(0xEB000004, 0x4000, 0x4004, ReadNothing, nullptr).ok);

    // things that read the PC in other ways are refused rather than miscompiled
    CHECK(!RelocateArm(0xE08F0001, 0x1000, 0x1004, ReadNothing, nullptr).ok); // add r0, pc, r1
    r = RelocateArm(0xE1A0100F, 0x1000, 0x1004, ReadNothing, nullptr); // mov r1, pc -> constant
    CHECK(r.ok && r.count == 4 && r.words[3] == 0x1008 && r.words[0] == 0xE59F1004);
    CHECK(!RelocateArm(0xE1B0100F, 0x1000, 0x1004, ReadNothing, nullptr).ok); // movs r1, pc
    CHECK(!RelocateArm(0xE58FF000, 0x1000, 0x1004, ReadNothing, nullptr).ok); // str pc, [pc]
    CHECK(!RelocateArm(0xE59FF004, 0x1000, 0x1004, ReadNothing, nullptr).ok == false); // ldr pc,[pc,#4] ok
    CHECK(!RelocateArm(0xE12FFF3E, 0x1000, 0x1004, ReadNothing, nullptr).ok);          // blx lr
}

static void SweepGame(const char* path, u32 text_size) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("(no game code at %s, sweep skipped)\n", path);
        return;
    }
    std::vector<u8> code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text_size == 0 || text_size > code.size()) {
        text_size = static_cast<u32>(code.size());
    }
    for (u32 svc : {0x32u, 0x23u}) {
        const u32 pattern = 0xEF000000 | svc;
        int total = 0, ok = 0;
        std::map<std::string, int> reasons;
        std::vector<u32> bad;
        for (u32 off = 0; off + 8 <= text_size; off += 4) {
            if (LoadLE32(&code[off]) != pattern) {
                continue;
            }
            ++total;
            const u32 address = 0x00100000 + off;
            const u32 next = LoadLE32(&code[off + 4]);
            RelocResult r = RelocateArm(next, address + 4, address + 8, ReadFromVector, &code);
            if (r.ok) {
                ++ok;
            } else {
                ++reasons[r.reason];
                if (bad.size() < 5) {
                    bad.push_back(address);
                }
            }
        }
        std::printf("game sweep: svc 0x%02x sites %d, relocatable %d\n", svc, total, ok);
        for (const auto& kv : reasons) {
            std::printf("    refused (%d): %s\n", kv.second, kv.first.c_str());
        }
        for (u32 a : bad) {
            std::printf("    e.g. at 0x%08x next=0x%08x\n", a, LoadLE32(&code[a - 0x00100000 + 4]));
        }
        CHECK(total == 0 || ok == total);
    }
}

int main(int argc, char** argv) {
    TestSynthetic();
    if (argc > 1) {
        SweepGame(argv[1], argc > 2 ? static_cast<u32>(std::strtoul(argv[2], nullptr, 0)) : 0);
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
