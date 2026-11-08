// Hooks a game's code image the way ctr/hook.cpp does on the console, with the very WriteGate of azl/gate_plan.hpp: the real gate
// (Sources/ctr/gate.s, assembled by Tests/hook_sim/build_gate.sh) copied into its page just below 0x01E80000, one stub per call site,
// and ONE word `b stub` at every ARM `svc 0x32` / `svc 0x23` of the text. On a file, so that an ARM emulator can run the hooked code
// against the original one (Tests/hook_sim/run_sim.py).
//
//   patch_image <code.bin> <text-size> <gate-dir> <out-dir>        (gate-dir from build_gate.sh: gate.bin, gate.sym)
// Writes: patched.bin (the code), page.bin (the gate's page, flag = 1 as WriteGate leaves it), layout.txt (where the page is and the
// pre/post its header points at), sites.txt (address kind). Fails when a site is left out, or when anything in the text changed but
// the sites' words.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "azl/gate_plan.hpp"

using namespace azl;

namespace {

constexpr u32 kBase = 0x00100000;
// pre/post of the stand-in plugin: run_sim.py hooks these addresses (the plugin's block, as on the console)
constexpr u32 kPre32 = 0x07000000;
constexpr u32 kPost32 = 0x07000010;
constexpr u32 kPre23 = 0x07000020;
constexpr u32 kPost23 = 0x07000030;

struct ImageMem {
    std::vector<u8>& image;
    std::vector<u8>& page;
    u32 page_base;
    u32 Load(u32 a) { return LoadLE32(&image[a - kBase]); }
    void StorePage(u32 a, u32 w) { StoreLE32(&page[a - page_base], w); }
    bool PatchText(u32 a, u32 w) {
        StoreLE32(&image[a - kBase], w);
        return true;
    }
    void Sync() {}
};

std::vector<u8> ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s code.bin text-size gate-dir out-dir\n", argv[0]);
        return 2;
    }
    std::vector<u8> image = ReadFile(argv[1]);
    const u32 text = static_cast<u32>(std::strtoul(argv[2], nullptr, 0));
    const std::string gate_dir = argv[3];
    const std::string out = argv[4];
    if (text > image.size()) {
        std::fprintf(stderr, "the text (0x%x bytes) is longer than the image (0x%zx)\n", text, image.size());
        return 2;
    }

    // the gate as the plugin carries it: its bytes, and its two entries from azl_gate_begin (the symbols of the assembled gate)
    const std::vector<u8> gate = ReadFile(gate_dir + "/gate.bin");
    std::map<std::string, u32> sym;
    {
        std::ifstream in(gate_dir + "/gate.sym");
        std::string name, value;
        while (in >> name >> value) {
            sym[name] = static_cast<u32>(std::strtoul(value.c_str(), nullptr, 16));
        }
    }
    if (gate.empty() || gate.size() % 4 != 0 || !sym.count("azl_gate_begin") || !sym.count("azl_gate_32") || !sym.count("azl_gate_23")) {
        std::fprintf(stderr, "no gate in %s (run build_gate.sh)\n", gate_dir.c_str());
        return 2;
    }
    std::vector<u32> gate_words(gate.size() / 4);
    for (size_t i = 0; i < gate_words.size(); ++i) {
        gate_words[i] = LoadLE32(&gate[4 * i]);
    }
    const GateBlob blob{gate_words.data(), static_cast<u32>(gate.size()), sym["azl_gate_32"] - sym["azl_gate_begin"],
                        sym["azl_gate_23"] - sym["azl_gate_begin"]};

    // the sites, as InstallSvcHooks finds them: every ARM word of the text that is `svc 0x32` or `svc 0x23`
    std::vector<GateSite> sites;
    for (u32 off = 0; off + 4 <= text; off += 4) {
        const u32 w = LoadLE32(&image[off]);
        if (w == kGateSvc32Word || w == kGateSvc23Word) {
            sites.push_back({kBase + off, w});
        }
    }
    if (sites.empty()) {
        std::fprintf(stderr, "no svc 0x32 / 0x23 in the text\n");
        return 1;
    }
    const std::vector<u8> original = image;
    const u32 pages = PageCount(static_cast<u32>(sites.size()), blob.bytes);
    const u32 page_base = kGateCeiling - pages * kGatePageBytes;
    std::vector<u8> page(pages * kGatePageBytes, 0);
    ImageMem mem{image, page, page_base};
    const GateWriteResult r = WriteGate(mem, page_base, pages * kGatePageBytes, blob, GatePointers{kPre32, kPost32, kPre23, kPost23},
                                        sites.data(), static_cast<u32>(sites.size()));

    // nothing else of the text moved: the words that differ are exactly the hooked sites, each `b` to its own stub
    const u32 stubs = StubsBase(page_base, blob.bytes);
    u32 changed = 0, wrong = 0;
    for (u32 off = 0; off + 4 <= image.size(); off += 4) {
        if (LoadLE32(&image[off]) != LoadLE32(&original[off])) {
            ++changed;
        }
    }
    std::string lines;
    for (u32 i = 0; i < r.hooked; ++i) {
        u32 jump = 0;
        if (!EncodeBranch(sites[i].address, stubs + i * kGateStubBytes, &jump) || LoadLE32(&image[sites[i].address - kBase]) != jump) {
            ++wrong;
        }
        char line[64];
        std::snprintf(line, sizeof(line), "%08x %s\n", sites[i].address, sites[i].original == kGateSvc32Word ? "send" : "close");
        lines += line;
    }
    std::ofstream(out + "/patched.bin", std::ios::binary).write(reinterpret_cast<const char*>(image.data()), image.size());
    std::ofstream(out + "/page.bin", std::ios::binary).write(reinterpret_cast<const char*>(page.data()), page.size());
    std::ofstream(out + "/sites.txt") << lines;
    {
        std::ofstream layout(out + "/layout.txt");
        char text_line[256];
        std::snprintf(text_line, sizeof(text_line),
                      "page %08x\npages %u\nstubs %08x\nsites %u\ntext %08x\npre32 %08x\npost32 %08x\npre23 %08x\npost23 %08x\n", page_base,
                      pages, stubs, r.hooked, text, kPre32, kPost32, kPre23, kPost23);
        layout << text_line;
    }
    std::printf("sites: %zu, hooked: %u, out of reach %u, foreign %u, no room %u, patch failed %u | page 0x%08x, %u page(s) | "
                "text words changed: %u, wrong branches: %u\n",
                sites.size(), r.hooked, r.out_of_reach, r.foreign, r.no_room, r.patch_failed, page_base, pages, changed, wrong);
    const bool ok = r.ok && r.hooked == sites.size() && r.patch_failed == 0 && changed == r.hooked && wrong == 0;
    return ok ? 0 : 1;
}
