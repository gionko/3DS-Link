// The decisions behind "is this game's plugin the one in 3DS Link's folder?" (azl/plugin_compare.hpp): the version word of a .3gx header, the version word of the plugin's own text,
// and what the two make of each other. The reading of the two files (plugin.cpp) is on the console.
//
//   plugin_compare_test      (needs nothing; built by Tests/run.sh)
#include <cstdio>
#include <cstring>

#include "azl/plugin_compare.hpp"

using namespace azl;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failures;                                                          \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                          \
    } while (0)

int main() {
    // the header of the 1.0.8 plugin as the console wrote it: "3GX$0002", then the version bytes 00 08 00 01 (revision, minor, major)
    const u8 head108[16] = {'3', 'G', 'X', '$', '0', '0', '0', '2', 0x00, 0x08, 0x00, 0x01, 0, 0, 0, 0};
    CHECK(PluginHeaderVersionWord(head108, 16) == 0x01000800u);
    CHECK(PluginHeaderVersionWord(head108, 12) == 0x01000800u);
    CHECK(PluginHeaderVersionWord(head108, 11) == 0); // too short
    CHECK(PluginHeaderVersionWord(nullptr, 16) == 0);
    u8 bad[16];
    std::memcpy(bad, head108, 16);
    bad[3] = '#'; // not the magic
    CHECK(PluginHeaderVersionWord(bad, 16) == 0);
    bad[3] = '$';
    bad[0] = '4';
    CHECK(PluginHeaderVersionWord(bad, 16) == 0);

    // the text of kVersion, the same word
    CHECK(PluginVersionWordFromText("1.0.8") == 0x01000800u);
    CHECK(PluginVersionWordFromText("1.0.8") == PluginHeaderVersionWord(head108, 16)); // (the two ways of writing the same version agree)
    CHECK(PluginVersionWordFromText("1.2.3") == 0x01020300u);
    CHECK(PluginVersionWordFromText("0.1.16") == 0x00011000u);
    CHECK(PluginVersionWordFromText("255.255.255") == 0xFFFFFF00u);
    CHECK(PluginVersionWordFromText("256.0.0") == 0);
    CHECK(PluginVersionWordFromText("1.0") == 0);
    CHECK(PluginVersionWordFromText("1.0.") == 0);
    CHECK(PluginVersionWordFromText("1.0.8x") == 0);
    CHECK(PluginVersionWordFromText("a.b.c") == 0);
    CHECK(PluginVersionWordFromText("") == 0);
    CHECK(PluginVersionWordFromText(nullptr) == 0);

    // what the two make of each other
    const u32 v108 = 0x01000800u, v109 = 0x01000900u, v1010 = 0x01000A00u;
    CHECK(RelateMaster(v108, v108, true) == MasterPlugin::Same);
    CHECK(RelateMaster(v108, v108, false) == MasterPlugin::OtherBuild);
    CHECK(RelateMaster(v108, v109, false) == MasterPlugin::Newer);
    CHECK(RelateMaster(v108, v109, true) == MasterPlugin::Newer); // (the bytes are not compared when the versions differ: they cannot be equal)
    CHECK(RelateMaster(v109, v108, false) == MasterPlugin::Older);
    CHECK(RelateMaster(v108, v1010, false) == MasterPlugin::Newer); // (1.0.10 is later than 1.0.8: the words are compared, not the text)
    CHECK(RelateMaster(v108, 0, true) == MasterPlugin::Unreadable);
    CHECK(RelateMaster(0, v108, true) == MasterPlugin::Unreadable);
    // which of them say something
    CHECK(MasterPluginWarns(MasterPlugin::Newer));
    CHECK(MasterPluginWarns(MasterPlugin::OtherBuild));
    CHECK(!MasterPluginWarns(MasterPlugin::Same));
    CHECK(!MasterPluginWarns(MasterPlugin::Older));
    CHECK(!MasterPluginWarns(MasterPlugin::Unreadable));

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
