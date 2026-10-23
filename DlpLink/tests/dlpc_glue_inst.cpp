// What the 3DS Link Installer's own reading code (DlpLink/child/source/installer_logic.hpp, the very file) makes of a composed child: the child as a CIA, the
// app's CIA that is in its RomFS, the manifest, the plugin. dlpc_test.c extracts those from the RomFS with its own reader and hands them here.
#include <cstdio>
#include <cstring>
#include <string>

#include "../child/source/installer_logic.hpp"

// 0 when the installer would read everything as it should; else the number of checks that failed, with the first one in `msg`.
extern "C" int dlpc_glue_inst(const unsigned char* child, size_t child_n, const unsigned char* app, size_t app_n, const char* manifest, size_t manifest_n,
                              const unsigned char* plugin, size_t plugin_n, unsigned long long child_tid, unsigned long long app_tid, unsigned version16,
                              const char* version_text, char* msg, size_t msg_size) {
    int failed = 0;
    msg[0] = 0;
    auto bad = [&](const char* what) {
        if (failed++ == 0) {
            std::snprintf(msg, msg_size, "%s", what);
        }
    };
    inst::CiaInfo info;
    // exactly what the installer reads of a CIA: the first 0x4000 bytes and the size of the file
    if (!inst::ParseCia(child, child_n < 0x4000 ? child_n : 0x4000, child_n, &info)) {
        bad("ParseCia refuses the child");
    } else {
        if (info.tid != child_tid) bad("child: title id");
        if (info.version != version16) bad("child: version");
        if (info.content_count != 1) bad("child: content count");
        if (info.total_size > child_n + 0x40 || info.total_size + 0x40 < child_n) bad("child: the header does not add up to the file");
    }
    if (!inst::ParseCia(app, app_n < 0x4000 ? app_n : 0x4000, app_n, &info)) {
        bad("ParseCia refuses the app's CIA of the RomFS");
    } else {
        if (info.tid != app_tid) bad("app: title id");
        if (info.version != version16) bad("app: version");
        if (inst::Decide(false, 0, info.version) != inst::Action::Install || inst::Decide(true, info.version, info.version) != inst::Action::Keep) bad("policy");
    }
    const inst::Manifest m = inst::ParseManifest(std::string(manifest, manifest_n));
    if (!m.complete()) bad("the manifest is not complete");
    if (m.tid != app_tid) bad("manifest: tid");
    if (m.version16 != version16) bad("manifest: version16");
    if (m.version != version_text) bad("manifest: version");
    if (m.cia_size != app_n) bad("manifest: size of the app's CIA");
    if (m.plugin_size != plugin_n) bad("manifest: size of the plugin");
    if (!inst::PluginHeaderOk(plugin, plugin_n)) bad("the plugin has no 3GX$ magic");
    if (inst::VersionText(version16) != version_text) bad("version text");
    return failed;
}
