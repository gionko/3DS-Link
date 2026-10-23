// Applies the svc hook to a game's code image exactly like ctr/hook.cpp does on the console, but
// on a file, so that an ARM emulator can run the patched code (Tests/hook_sim/run_sim.py).
//
//   patch_image <code.bin> <text-size> <out-dir> <azl_svc32_entry addr> <azl_svc23_entry addr>
// Writes: patched.bin (the code), tramps.bin (the trampoline area), sites.txt (address kind)
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "azl/svc_patch.hpp"

using namespace azl;

constexpr u32 kBase = 0x00100000;
constexpr u32 kTrampBase = 0x07100000;

static u32 ReadImage(void* ctx, u32 address) {
    const auto* img = static_cast<const std::vector<u8>*>(ctx);
    return LoadLE32(img->data() + (address - kBase));
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s code.bin text-size out-dir\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<u8> image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const u32 text = static_cast<u32>(std::strtoul(argv[2], nullptr, 0));
    const std::string out = argv[3];
    const u32 kEntry32 = static_cast<u32>(std::strtoul(argv[4], nullptr, 16));
    const u32 kEntry23 = static_cast<u32>(std::strtoul(argv[5], nullptr, 16));

    const std::vector<u8> original = image; // literals are read from the unpatched image
    std::vector<u8> tramps;
    std::string sites;
    int built = 0, skipped = 0;
    for (u32 off = 0; off + 8 <= text; off += 4) {
        const u32 w = LoadLE32(&original[off]);
        if (w != 0xEF000032 && w != 0xEF000023) {
            continue;
        }
        const u32 site = kBase + off;
        const u32 tramp = kTrampBase + static_cast<u32>(tramps.size());
        u32 words[kWordsPerTrampoline];
        const char* reason = "";
        if (!BuildSvcTrampoline(site, LoadLE32(&original[off + 4]), tramp,
                                w == 0xEF000032 ? kEntry32 : kEntry23, ReadImage,
                                const_cast<std::vector<u8>*>(&original), words, &reason)) {
            ++skipped;
            continue;
        }
        for (u32 word : words) {
            u8 b[4];
            StoreLE32(b, word);
            tramps.insert(tramps.end(), b, b + 4);
        }
        StoreLE32(&image[off], kSiteJumpWord);
        StoreLE32(&image[off + 4], tramp);
        char line[64];
        std::snprintf(line, sizeof(line), "%08x %s\n", site, w == 0xEF000032 ? "send" : "close");
        sites += line;
        ++built;
    }
    std::ofstream(out + "/patched.bin", std::ios::binary).write(reinterpret_cast<char*>(image.data()), image.size());
    std::ofstream(out + "/tramps.bin", std::ios::binary).write(reinterpret_cast<char*>(tramps.data()), tramps.size());
    std::ofstream(out + "/sites.txt") << sites;
    std::printf("trampolines built: %d, skipped: %d\n", built, skipped);
    return skipped ? 1 : 0;
}
