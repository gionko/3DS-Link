#include "system.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "settings.hpp"

namespace setup {

namespace {

// Fills the size, the magic number and the version of the header.
void DescribeFile(PluginFile& file) {
    const FileInfo info = StatFile(file.path.c_str());
    file.size = info.size;
    file.header_ok = false;
    FILE* f = std::fopen(file.path.c_str(), "rb");
    if (f) {
        char head[1024] = {};
        const std::size_t got = std::fread(head, 1, sizeof(head), f);
        std::fclose(f);
        if (got >= 4) {
            file.header_ok = std::memcmp(head, "3GX$", 4) == 0;
        }
        if (file.header_ok && got >= 12) {
            std::memcpy(&file.version, head + 8, 4); // after the magic and the format number: the plugin's version
        }
    }
}

} // namespace

NetInfo ReadNetwork(bool ac_ready, bool soc_ready) {
    NetInfo net;
    if (!ac_ready) {
        return net;
    }
    u32 status = 0;
    if (R_SUCCEEDED(ACU_GetWifiStatus(&status))) {
        net.known = true;
        net.wifi = status != 0;
    }
    net.strength = osGetWifiStrength();
    if (net.wifi && soc_ready) {
        in_addr address;
        address.s_addr = static_cast<in_addr_t>(gethostid());
        if (address.s_addr != 0) {
            net.ip = inet_ntoa(address);
        }
    }
#ifdef AZL_TEST_WIFI_OFF
    net.known = true; // (a build made to look at the "not connected" line on an emulator that is always connected)
    net.wifi = false;
    net.strength = 0;
    net.ip.clear();
#endif
    return net;
}

namespace {
bool g_ptmu_open = false;
bool g_mcu_open = false;

// The port of Luma3DS's Plugin Loader service. (A build made to try the "service is missing" branch on an emulator that has the
// service gives another name here: make EXTRA_CFLAGS='-DAZL_PLG_PORT="plg:none"'.)
#ifndef AZL_PLG_PORT
#define AZL_PLG_PORT "plg:ldr"
#endif
const char* const kPluginLoaderPort = AZL_PLG_PORT;
} // namespace

void OpenStatusServices() {
    g_ptmu_open = R_SUCCEEDED(ptmuInit());
    g_mcu_open = R_SUCCEEDED(mcuHwcInit()); // a 3dsx is often refused here: ptm:u then gives the five steps of the symbol
}

void CloseStatusServices() {
    if (g_mcu_open) {
        mcuHwcExit();
        g_mcu_open = false;
    }
    if (g_ptmu_open) {
        ptmuExit();
        g_ptmu_open = false;
    }
}

void ReadConsoleStatus(ConsoleStatus& out, bool with_sd) {
    out.battery_percent = -1;
    out.battery_steps = -1;
    out.charging = false;
    u8 value = 0;
    if (g_mcu_open && R_SUCCEEDED(MCUHWC_GetBatteryLevel(&value))) {
        out.battery_percent = value;
    }
    if (g_ptmu_open) {
        if (R_SUCCEEDED(PTMU_GetBatteryLevel(&value))) {
            out.battery_steps = value;
        }
        if (R_SUCCEEDED(PTMU_GetBatteryChargeState(&value))) {
            out.charging = value != 0;
        }
    }
    if (with_sd) {
        FS_ArchiveResource resource;
        out.sd_known = R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&resource));
        if (out.sd_known) {
            out.sd_total = static_cast<u64>(resource.totalClusters) * resource.clusterSize;
            out.sd_free = static_cast<u64>(resource.freeClusters) * resource.clusterSize;
        }
    }
}

// The Plugin Loader's commands, as Luma3DS's sysmodules/rosalina/source/plugin/plgloader.c (PluginLoader__HandleCommands)
// takes them, and as its client in plgldr.c and CTRPluginFramework's plgldr.c send them (checked against all three):
//   port "plg:ldr"  (rosalina main.c: { "plg:ldr", 1, PluginLoader__HandleCommands, true })
//   command 2  request IPC_MakeHeader(2, 0, 0)     answer IPC_MakeHeader(2, 2, 0): word 1 = result, word 2 = enabled (0 / 1)
//   command 3  request IPC_MakeHeader(3, 1, 0), word 1 = enabled (0 / 1)
//                                                  answer IPC_MakeHeader(3, 1, 0): word 1 = result
// A request with another header is answered with 0xD9001830, which reads as "unknown" here. No other command is ever sent.
LoaderState ReadPluginLoader() {
    Handle port = 0;
    if (R_FAILED(svcConnectToPort(&port, kPluginLoaderPort))) {
        return LoaderState::Unknown; // not there: the answer is immediate, not a wait
    }
    LoaderState state = LoaderState::Unknown;
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(2, 0, 0);
    Result rc = svcSendSyncRequest(port);
    if (R_SUCCEEDED(rc)) {
        rc = cmdbuf[1];
        if (R_SUCCEEDED(rc)) {
            state = cmdbuf[2] != 0 ? LoaderState::Enabled : LoaderState::Disabled;
        }
    }
#ifdef AZL_TEST_LOADER_DISABLED
    if (R_SUCCEEDED(rc)) {
        state = LoaderState::Disabled; // (a build made to look at the question on an emulator whose loader always says "enabled")
    }
#endif
    svcCloseHandle(port);
    return state;
}

Result SetPluginLoader(bool enabled) {
    Handle port = 0;
    Result rc = svcConnectToPort(&port, kPluginLoaderPort);
    if (R_FAILED(rc)) {
        return rc;
    }
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(3, 1, 0);
    cmdbuf[1] = enabled ? 1u : 0u;
    rc = svcSendSyncRequest(port);
    if (R_SUCCEEDED(rc)) {
        rc = cmdbuf[1];
    }
    svcCloseHandle(port);
    return rc;
}

SeedReading ReadSeedDb() {
    SeedReading reading;
    reading.state = 2;
    reading.rc = -1;
#ifdef AZL_TEST_SEED_READABLE
    reading.state = 1; // (a build made to look at the check on an emulator, which has no such file)
    reading.size = 31 * 1024;
    reading.rc = 0;
    return reading;
#endif
    // The ID0 of the console is the name of the folder its titles are in on the card: sdmc:/Nintendo 3DS/<ID0>, 32 hex digits.
    std::string id0;
    if (DIR* dir = opendir("sdmc:/Nintendo 3DS")) {
        while (const dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            bool hex = name.size() == 32;
            for (const char c : name) {
                hex = hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
            }
            if (hex) {
                id0 = name;
                break;
            }
        }
        closedir(dir);
    }
    if (id0.empty()) {
        return reading;
    }
    const std::string path = "/data/" + id0 + "/sysdata/0001000f/00000000";
    static const FS_ArchiveID archives[] = {ARCHIVE_NAND_RO, ARCHIVE_NAND_CTR_FS}; // (the two places the module looks in)
    for (const FS_ArchiveID archive : archives) {
        Handle file = 0;
        const Result rc = FSUSER_OpenFileDirectly(&file, archive, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, path.c_str()), FS_OPEN_READ, 0);
        if (R_FAILED(rc)) {
            reading.rc = rc;
            continue;
        }
        u64 size = 0;
        const bool ok = R_SUCCEEDED(FSFILE_GetSize(file, &size)) && size > 0 && size <= 0x800000;
        FSFILE_Close(file);
        if (ok) {
            reading.state = 1;
            reading.size = size;
            reading.rc = 0;
            return reading;
        }
    }
    return reading;
}

namespace {

// Can the data folder be written? The test writes a file and removes it, and the answer is asked for at every return to the main page: a card
// that could be written a moment ago is not tested again for a minute (one that could not is tested every time, to see it come back).
bool SdWritable(const std::string& dir) {
    static std::string tested_dir;
    static u64 tested_at = 0;
    const u64 now = osGetTime();
    if (tested_at != 0 && tested_dir == dir && now - tested_at < 60000) {
        return true;
    }
    if (!ProbeWritable(dir.c_str())) {
        tested_at = 0;
        return false;
    }
    tested_dir = dir;
    tested_at = now;
    return true;
}

} // namespace

InstallFacts ReadInstallFacts(const Paths& paths, const Boot9Lookup& boot9, LoaderState loader) {
    InstallFacts facts;
    bool exists = false;
    const PluginFile plugin = DescribePlugin(paths.plugin_file, &exists);
    facts.plugin_path = paths.plugin_file;
    facts.plugin_size = plugin.size;
    facts.plugin_version = plugin.version;
    // (what the card holds against the plugin inside the app, byte for byte, is not read here: the app looks at that only when the file changed and keeps the answer,
    // ui.cpp App::LookAtPlugin, and fills facts.plugin_card, embedded_ok and the rest)
    facts.plugins_dir = PluginsDirExists();
    facts.default_plugin = StatFile(kDefaultPluginPath).exists;
    facts.boot9 = boot9.found;
    facts.boot9_path = boot9.path;
    facts.sd_writable = SdWritable(paths.datadir);
    facts.app_dir = paths.appdir;
    const SeedReading seed = ReadSeedDb();
    facts.seed = seed.state;
    facts.seed_size = seed.size;
    facts.seed_error = static_cast<u32>(seed.rc);
    facts.loader = loader;
    s64 luma = 0;
    if (R_SUCCEEDED(svcGetSystemInfo(&luma, 0x10000, 0))) { // Luma3DS's own query: an error on a system without it (and on an emulator)
        facts.luma = static_cast<u32>(luma);
    }
#ifdef AZL_TEST_LUMA
    // (a build made to look at the screens of an old Luma3DS on an emulator, e.g. -DAZL_TEST_LUMA=0x0a020100u for 10.2.1; the emulator answers the Plugin Loader,
    // an old Luma3DS does not)
    facts.luma = AZL_TEST_LUMA;
    facts.loader = LoaderState::Unknown;
#endif
    return facts;
}

// The same rule the plugin applies (Plugin SanitizeNickname): the room only accepts
// ^[ a-zA-Z0-9._-]{4,20}$, so anything else in the profile name becomes '_'.
std::string ReadConsoleName() {
    std::string name;
    u16 raw[0x1C / 2 + 1] = {};
    if (R_SUCCEEDED(CFGU_GetConfigInfoBlk2(0x1C, 0x000A0000, raw))) {
        for (int i = 0; i < 10 && raw[i]; ++i) {
            const u16 c = raw[i];
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                            c == ' ' || c == '.' || c == '_' || c == '-';
            name += ok ? static_cast<char>(c) : '_';
        }
    }
    while (!name.empty() && name.front() == ' ') {
        name.erase(name.begin());
    }
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }
    bool meaningful = false;
    for (char c : name) {
        if (c != '_' && c != ' ' && c != '.' && c != '-') {
            meaningful = true;
        }
    }
    if (!meaningful) {
        return FallbackConsoleName();
    }
    if (name.size() < 4) {
        name += " 3DS";
    }
    return name;
}

std::string FallbackConsoleName() {
    const azl::Mac mac = ReadConsoleMac();
    char name[16];
    std::snprintf(name, sizeof(name), "3DS %02X%02X", mac[4], mac[5]);
    return name;
}

azl::Mac ReadConsoleMac() {
    azl::Mac mac{};
    std::memcpy(mac.data(), reinterpret_cast<const void*>(0x1FF81060), 6); // shared page: WiFi MAC
    return mac;
}

std::string MacToString(const azl::Mac& mac) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
                  mac[4], mac[5]);
    return buf;
}

FileInfo StatFile(const char* path) {
    FileInfo info;
    struct stat st;
    if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) {
        info.exists = true;
        info.size = static_cast<u64>(st.st_size);
    }
    return info;
}

PluginFile DescribePlugin(const std::string& path, bool* exists) {
    PluginFile file;
    file.path = path;
    const bool there = StatFile(path.c_str()).exists;
    if (exists) {
        *exists = there;
    }
    if (there) {
        DescribeFile(file);
    }
    return file;
}

} // namespace setup
