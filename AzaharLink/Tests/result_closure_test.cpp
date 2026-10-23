// Result-set closure (spec C10): whatever a game sends, the result UdsHle answers must be 0 or one of
// the codes the firmware itself can return for that command (spec T1 / section 2). Every command id
// 1..0x23 plus 0 and 0x24, in a few argument variants each, is sent through UdsHle in each state the
// service can be in: S0 (never initialised / after Shutdown), S3 idle, S6 host, S7 connecting, S9
// client, S10 spectator, and the service states SV1 (wireless off) / SV2 (sleep).
//
// Any result outside the set fails, and so does any frame that breaks an Azahar-peer rule
// (azahar_rules.hpp). A source scan fails the test if a constant 0.1.14 invented (the codes the
// firmware never returns) comes back into Sources/ or Includes/. command_matrix_test.cpp is the
// stricter sibling: the exact result of every cell.
//
//   result_closure_test                              (run from the repository root)
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "azl/log.hpp"

#include "hle_states.hpp"

using namespace azl;

// ---------------------------------------------------------------------------------------------
// The firmware result set of every command (spec section 2, T1): the codes it can return besides 0
// ---------------------------------------------------------------------------------------------
static std::set<u32> AllowedResults(u32 id) {
    using namespace fw;
    switch (id) {
    case 0x01:
    case 0x1B:
        return {kW, kWO, kOom, kInitFail};
    case 0x02:
    case 0x03:
    case 0x0B:
    case 0x23:
        return {};
    case 0x04:
    case 0x1D: // an invalid network is a fatal error in nwm: the HLE keeps a usage code
        return {kW, kWO, kArg, kBadId, kBig};
    case 0x05:
        return {kW, kWO, kBadId};
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x0A:
    case 0x0C:
    case 0x11:
    case 0x1A:
    case 0x1C:
    case 0x20:
        return {kW, kWO};
    case 0x09:
    case 0x1E:
        return {kW, kWO, kArg, kNoHost, kConnFailed, kConnTimeout, kNetFull};
    case 0x0D:
        return {kNodeNotFound, kW, kWO};
    case 0x0E:
    case 0x1F:
        return {kBadBeacon, kW, kWO};
    case 0x0F:
        return {kW, kWO};
    case 0x10:
        return {kW, kWO, kBig};
    case 0x12:
        return {kW, kWO, kBadId, kOom};
    case 0x13:
        return {kBadId};
    case 0x14:
        return {kW, kWO, kBadId, kBig};
    case 0x15:
    case 0x16:
        return {kW, kWO, kArg};
    case 0x17:
        return {kW, kWO, kBig, kAgg, kNodeNotFound};
    case 0x18:
    case 0x19:
        return {kW, kArg};
    case 0x21:
        return {kW};
    case 0x22:
        return {kW, kWO, kArg};
    default: // 0 and everything above 0x23: the dispatcher itself
        return {kBadCmd, kBadHeader};
    }
}

// ---------------------------------------------------------------------------------------------
// Source scan: the codes 0.1.14 invented must stay out of the product
// ---------------------------------------------------------------------------------------------
static std::set<std::string> ScanSources() {
    namespace fs = std::filesystem;
    static const char* const kTokens[] = {"0xC8A111EA", "0xE0A113EA", "0xC94113F8", "kResWrongStatus",
                                          "kResNotAuthorizedUsageState", "kResNotInitialized"};
    std::set<std::string> found;
    for (const char* root : {"Sources", "Includes"}) {
        if (!fs::exists(root)) {
            std::fprintf(stderr, "  FAIL source scan: no %s directory (run from the repository root)\n", root);
            ++g_failures;
            continue;
        }
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string ext = entry.path().extension().string();
            if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".c" && ext != ".s") {
                continue;
            }
            std::ifstream in(entry.path(), std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            std::string text = ss.str();
            for (char& c : text) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            for (const char* token : kTokens) {
                std::string upper = token;
                for (char& c : upper) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                if (text.find(upper) != std::string::npos) {
                    found.insert("scan " + entry.path().generic_string() + " " + token);
                }
            }
        }
    }
    return found;
}

int main() {
    SetLogSink([](const char*) {});
    std::printf("result-set closure: every command x S0 S3 S6 S7 S9 S10 SV1 SV2\n");
    std::set<std::string> violations;
    int cells = 0;
    const std::vector<Variant> variants = Variants();
    Watchdog dog(30); // a cell that hangs is a bug in the engine or the HLE: name it and abort
    const bool verbose = std::getenv("CLOSURE_VERBOSE") != nullptr;
    for (const State state : AllStates()) {
        for (const Variant& variant : variants) {
            dog.Step(CellName("cell", state, variant));
            if (verbose) {
                std::fprintf(stderr, "  %s\n", CellName("cell", state, variant).c_str());
            }
            g_rules.Reset();
            Env env;
            const u32 result = RunCell(env, state, variant).result();
            ++cells;
            if (result != 0 && AllowedResults(variant.id).count(result) == 0) {
                char tail[16];
                std::snprintf(tail, sizeof(tail), " 0x%08lX", static_cast<unsigned long>(result));
                violations.insert(CellName("result", state, variant) + tail);
            }
            // The frames the command made the engine send must keep to the Azahar-peer rules
            for (const AzaharRules::Violation& rule : g_rules.violations()) {
                violations.insert(CellName("rules", state, variant) + " " + rule.rule);
            }
        }
    }
    dog.Step("");
    std::printf("  %d cells run\n", cells);

    const std::set<std::string> scan = ScanSources();
    violations.insert(scan.begin(), scan.end());
    for (const std::string& v : violations) {
        std::fprintf(stderr, "  FAIL violation: %s\n", v.c_str());
    }
    g_checks += cells;
    g_failures += static_cast<int>(violations.size());
    std::printf("  %zu violations\n", violations.size());
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
