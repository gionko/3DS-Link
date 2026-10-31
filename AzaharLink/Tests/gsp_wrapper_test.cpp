// Tests for the GSP wrapper that the framework looks for (Includes/azl/gsp_wrapper.hpp): the wrapper whose `svc 0x32` is behind a stub.
//   gsp_wrapper_test [path-to-decompressed-.code] [text-size] [expected-bl-address]
// Without arguments only the synthetic cases run. With a game's decompressed .code (loaded at 0x00100000) the same search runs over the whole
// image and, if an expected address is given, it must find exactly that one (the address of the bl of LEGO Batman 2 is not written here).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#include "azl/gsp_wrapper.hpp"

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

constexpr u32 kBase = 0x00100000;
constexpr u32 kNop = 0xE320F000;

struct Image {
    std::vector<u8> bytes;
    explicit Image(u32 size) : bytes(size) {
        for (u32 i = 0; i < size; i += 4) {
            Put(kBase + i, kNop);
        }
    }
    void Put(u32 address, u32 word) {
        const u32 o = address - kBase;
        for (int i = 0; i < 4; ++i) {
            bytes[o + i] = static_cast<u8>(word >> (8 * i));
        }
    }
    u32 Get(u32 address) const {
        const u32 o = address - kBase;
        return bytes[o] | (bytes[o + 1] << 8) | (bytes[o + 2] << 16) | (static_cast<u32>(bytes[o + 3]) << 24);
    }
    u32 End() const { return kBase + static_cast<u32>(bytes.size()); }
};

static u32 Read(void* context, u32 address) {
    const Image* image = static_cast<const Image*>(context);
    if (address < kBase || address + 4 > image->End()) {
        return 0; // never part of a pattern
    }
    return image->Get(address);
}

static u32 BlWord(u32 from, u32 to) {
    const u32 offset = ((to - (from + 8)) >> 2) & 0x00FFFFFF;
    return 0xEB000000 | offset;
}

static void PutStub(Image& image, u32 at, u32 first = kSvcSendSyncRequestWord, u32 second = kBxLrWord) {
    image.Put(at, first);
    image.Put(at + 4, second);
}

/// The libctru wrapper at `at`, with its svc behind a `bl` to `stub` (or inline when `stub` is 0).
static void PutWrapper(Image& image, u32 at, u32 stub) {
    for (u32 i = 0; i < kGspWrapperCtruWords; ++i) {
        image.Put(at + 4 * i, kGspWrapperCtru[i]);
    }
    if (stub) {
        image.Put(at + kGspWrapperCtruSvcOffset, BlWord(at + kGspWrapperCtruSvcOffset, stub));
    }
}

static u32 Find(const Image& image, u32 first, u32 end, u32* sites, u32 max = 4) {
    return FindStubbedGspWrappers(Read, const_cast<Image*>(&image), first, end, sites, max);
}

static void TestBlTarget() {
    u32 target = 0;
    CHECK(ArmBlTarget(BlWord(0x00101000, 0x00180000), 0x00101000, &target) && target == 0x00180000); // forward
    CHECK(ArmBlTarget(BlWord(0x00200000, 0x00100000), 0x00200000, &target) && target == 0x00100000); // backward
    CHECK(ArmBlTarget(BlWord(0x00100000, 0x00100000), 0x00100000, &target) && target == 0x00100000); // itself
    CHECK(!ArmBlTarget((BlWord(0x00101000, 0x00180000) & 0x00FFFFFF) | 0x0B000000, 0x00101000, &target)); // bleq
    CHECK(!ArmBlTarget((BlWord(0x00101000, 0x00180000) & 0x00FFFFFF) | 0xEA000000, 0x00101000, &target)); // b
    CHECK(!ArmBlTarget(0xEF000032, 0x00101000, &target)); // svc
}

static void TestFind() {
    u32 sites[4] = {};

    // 1) the wrapper with a bl to the stub: found, at the bl (+0x2C)
    {
        Image image(0x1000);
        PutWrapper(image, kBase + 0x100, kBase + 0x800);
        PutStub(image, kBase + 0x800);
        CHECK(Find(image, kBase, image.End(), sites) == 1);
        CHECK(sites[0] == kBase + 0x100 + 0x2C);
    }
    // 2) the wrapper as the framework knows it (inline svc): nothing to do
    {
        Image image(0x1000);
        PutWrapper(image, kBase + 0x100, 0);
        PutStub(image, kBase + 0x800);
        CHECK(Find(image, kBase, image.End(), sites) == 0);
    }
    // 3) a stub before the wrapper (backward bl)
    {
        Image image(0x1000);
        PutStub(image, kBase + 0x40);
        PutWrapper(image, kBase + 0x100, kBase + 0x40);
        CHECK(Find(image, kBase, image.End(), sites) == 1 && sites[0] == kBase + 0x100 + 0x2C);
    }
    // 4) any other word of the pattern different: not the wrapper
    for (u32 i = 0; i < kGspWrapperCtruWords; ++i) {
        if (i == kGspWrapperCtruSvcIndex) {
            continue;
        }
        Image image(0x1000);
        PutWrapper(image, kBase + 0x100, kBase + 0x800);
        PutStub(image, kBase + 0x800);
        image.Put(kBase + 0x100 + 4 * i, image.Get(kBase + 0x100 + 4 * i) ^ 0x00000100);
        CHECK(Find(image, kBase, image.End(), sites) == 0);
    }
    // 5) the target is not exactly `svc 0x32 ; bx lr`
    {
        Image image(0x1000);
        PutWrapper(image, kBase + 0x100, kBase + 0x800);
        PutStub(image, kBase + 0x800, 0xEF000023, kBxLrWord); // svc 0x23
        CHECK(Find(image, kBase, image.End(), sites) == 0);
        PutStub(image, kBase + 0x800, kSvcSendSyncRequestWord, 0xE1A0F00E); // mov pc, lr
        CHECK(Find(image, kBase, image.End(), sites) == 0);
        PutStub(image, kBase + 0x800, kSvcSendSyncRequestWord, 0x012FFF1E); // bxeq lr
        CHECK(Find(image, kBase, image.End(), sites) == 0);
        PutStub(image, kBase + 0x800, kBxLrWord, kSvcSendSyncRequestWord); // the other way round
        CHECK(Find(image, kBase, image.End(), sites) == 0);
    }
    // 6) a conditional bl, or a plain b, is not the wrapper
    {
        Image image(0x1000);
        PutWrapper(image, kBase + 0x100, kBase + 0x800);
        PutStub(image, kBase + 0x800);
        image.Put(kBase + 0x100 + 0x2C, (image.Get(kBase + 0x100 + 0x2C) & 0x0FFFFFFF) | 0x00000000); // bleq
        CHECK(Find(image, kBase, image.End(), sites) == 0);
        image.Put(kBase + 0x100 + 0x2C, (BlWord(kBase + 0x100 + 0x2C, kBase + 0x800) & 0x00FFFFFF) | 0xEA000000); // b
        CHECK(Find(image, kBase, image.End(), sites) == 0);
    }
    // 7) the stub or the wrapper outside the range that is searched
    {
        Image image(0x1000);
        PutWrapper(image, kBase + 0x100, kBase + 0x800);
        PutStub(image, kBase + 0x800);
        CHECK(Find(image, kBase, kBase + 0x800, sites) == 0);          // the stub is past the end
        CHECK(Find(image, kBase, kBase + 0x808, sites) == 1);          // the stub is the last thing
        CHECK(Find(image, kBase, kBase + 0x804, sites) == 0);          // half of the stub
        CHECK(Find(image, kBase + 0x104, image.End(), sites) == 0);    // the wrapper starts before the range
        CHECK(Find(image, kBase, kBase + 0x100 + 4 * kGspWrapperCtruWords - 4, sites) == 0); // the wrapper is cut
        CHECK(Find(image, kBase + 0x100, image.End(), sites) == 1);    // the range starts at the wrapper
        CHECK(Find(image, kBase + 0x200, image.End(), sites) == 0);    // the wrapper is before the range
        Image back(0x1000);
        PutStub(back, kBase + 0x40);
        PutWrapper(back, kBase + 0x100, kBase + 0x40);
        CHECK(Find(back, kBase, back.End(), sites) == 1);
        CHECK(Find(back, kBase + 0x100, back.End(), sites) == 0);      // the stub is before the range
    }
    // 8) two wrappers, and the limit
    {
        Image image(0x1000);
        PutWrapper(image, kBase + 0x100, kBase + 0x800);
        PutWrapper(image, kBase + 0x300, kBase + 0x800);
        PutStub(image, kBase + 0x800);
        CHECK(Find(image, kBase, image.End(), sites, 4) == 2 && sites[0] == kBase + 0x12C && sites[1] == kBase + 0x32C);
        CHECK(Find(image, kBase, image.End(), sites, 1) == 1 && sites[0] == kBase + 0x12C);
        CHECK(Find(image, kBase, image.End(), sites, 0) == 0);
    }
    // 9) an unaligned or wild target is refused
    {
        Image image(0x1000);
        PutWrapper(image, kBase + 0x100, kBase + 0x800);
        PutStub(image, kBase + 0x800);
        image.Put(kBase + 0x100 + 0x2C, 0xEB7FFFFF); // far away, past the image
        CHECK(Find(image, kBase, image.End(), sites) == 0);
    }
    // 10) a game with no wrapper at all
    {
        Image image(0x1000);
        PutStub(image, kBase + 0x800);
        CHECK(Find(image, kBase, image.End(), sites) == 0);
    }
}

static void SweepGame(const char* path, u32 text_size, u32 expected) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        ++g_failures;
        return;
    }
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Image image(0);
    image.bytes.assign(raw.begin(), raw.end());
    const u32 end = kBase + (text_size ? text_size : static_cast<u32>(raw.size()));
    u32 sites[4] = {};
    const u32 found = Find(image, kBase, end, sites);
    std::printf("game sweep: %u GSP wrapper(s) behind a stub:", found);
    for (u32 i = 0; i < found; ++i) {
        std::printf(" bl at 0x%08x (wrapper 0x%08x)", sites[i], sites[i] - kGspWrapperCtruSvcOffset);
    }
    std::printf("\n");
    if (expected) {
        CHECK(found == 1 && sites[0] == expected);
        // what the framework would then see at the wrapper: its pattern, word by word
        if (found == 1) {
            for (u32 i = 0; i < kGspWrapperCtruWords; ++i) {
                const u32 want = kGspWrapperCtru[i];
                const u32 have = i == kGspWrapperCtruSvcIndex ? kSvcSendSyncRequestWord : image.Get(sites[0] - kGspWrapperCtruSvcOffset + 4 * i);
                CHECK(have == want);
            }
        }
    }
}

int main(int argc, char** argv) {
    TestBlTarget();
    TestFind();
    if (argc > 1) {
        SweepGame(argv[1], argc > 2 ? static_cast<u32>(std::strtoul(argv[2], nullptr, 0)) : 0,
                  argc > 3 ? static_cast<u32>(std::strtoul(argv[3], nullptr, 0)) : 0);
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
