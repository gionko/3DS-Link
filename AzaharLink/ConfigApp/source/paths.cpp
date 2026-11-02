#include "paths.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>

#include "games.hpp"

extern "C" {
#include "../titleshare/common/sha256.h"
}

namespace setup {

namespace {

Paths g_paths = Paths::FromAppDir(kDefaultAppDir);

bool IsDir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool FileSizeOf(const std::string& path, u64* size) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
        return false;
    }
    if (size) {
        *size = static_cast<u64>(st.st_size);
    }
    return true;
}

bool Exists(const std::string& path) {
    return FileSizeOf(path, nullptr);
}

std::string StripTrailingSlashes(std::string path) {
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

} // namespace

std::string AppDirFromArgv0(const char* argv0) {
    if (!argv0 || !*argv0) {
        return kDefaultAppDir;
    }
    std::string p = argv0;
    for (char& c : p) {
        c = c == '\\' ? '/' : c;
    }
    std::string rest;
    if (p.compare(0, 5, "sdmc:") == 0) {
        rest = p.substr(5);
    } else if (p[0] == '/') {
        rest = p; // (a launcher that leaves the device out: the SD card)
    } else {
        return kDefaultAppDir; // another device ("3dslink:/x.3dsx", "romfs:/...") or a bare name: nothing says where the SD card folder is
    }
    const bool names_a_folder = !rest.empty() && rest.back() == '/';
    std::vector<std::string> parts;
    std::string token;
    for (std::size_t i = 0; i <= rest.size(); ++i) {
        if (i < rest.size() && rest[i] != '/') {
            token += rest[i];
            continue;
        }
        if (token == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else if (!token.empty() && token != ".") {
            parts.push_back(token);
        }
        token.clear();
    }
    if (!names_a_folder && !parts.empty()) {
        parts.pop_back(); // the last part is 3DSLink.3dsx itself
    }
    std::string dir = "sdmc:";
    for (const std::string& part : parts) {
        dir += "/" + part;
    }
    return dir;
}

std::string ServersFileName(int list) {
    return list <= 1 ? std::string(kServersFileName) : "servers" + std::to_string(list) + ".txt";
}

std::string JoinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) {
        return name;
    }
    if (name.empty()) {
        return dir;
    }
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

std::string ParentDir(const std::string& path) {
    const std::string p = StripTrailingSlashes(path);
    const std::size_t slash = p.rfind('/');
    if (slash == std::string::npos) {
        return std::string(); // "sdmc:" or a bare name: no folder above
    }
    return slash == 0 ? std::string() : p.substr(0, slash);
}

std::string FileStamp(const std::string& path) {
    struct stat st;
    if (path.empty() || stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
        return std::string();
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%llu:%lld", static_cast<unsigned long long>(st.st_size), static_cast<long long>(st.st_mtime));
    return buf;
}

int RemoveParkFiles(const std::string& park_dir) {
    DIR* d = opendir(park_dir.c_str());
    if (!d) {
        return 0;
    }
    std::vector<std::string> names; // (collected first: a folder is not changed while it is being read)
    while (const struct dirent* entry = readdir(d)) {
        const std::string name = entry->d_name;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".blk") == 0) {
            names.push_back(name);
        }
    }
    closedir(d);
    int gone = 0;
    for (const std::string& name : names) {
        gone += std::remove(JoinPath(park_dir, name).c_str()) == 0 ? 1 : 0;
    }
    return gone;
}

std::string BaseName(const std::string& path) {
    const std::string p = StripTrailingSlashes(path);
    const std::size_t slash = p.rfind('/');
    if (slash == std::string::npos) {
        return p.empty() || p.back() == ':' ? std::string() : p;
    }
    return p.substr(slash + 1);
}

bool SameNameNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

Paths Paths::FromAppDir(const std::string& appdir) {
    Paths p;
    p.appdir = appdir;
    // The app's own folder is the folder it is in when that one is called 3DSLink (sdmc:/3ds/3DSLink/3DSLink.3dsx keeps everything in
    // sdmc:/3ds/3DSLink), otherwise a folder of that name inside it.
    p.datadir = SameNameNoCase(BaseName(appdir), kDataDirName) ? appdir : JoinPath(appdir, kDataDirName);
    p.plugin_beside = JoinPath(appdir, kPluginFileName);
    p.plugin_in_data = JoinPath(p.datadir, kPluginFileName);
    p.plugin_file = p.plugin_beside;
    p.boot9_beside = JoinPath(appdir, kBoot9FileName);
    p.boot9_in_data = JoinPath(p.datadir, kBoot9FileName);
    p.boot9_godmode = JoinPath(kGodModeOutDir, kBoot9FileName);
    p.config_file = JoinPath(p.datadir, kConfigFileName);
    p.servers_file = JoinPath(p.datadir, kServersFileName);
    p.games_cache = JoinPath(p.datadir, kGamesCacheFileName);
    p.catalog_cache = JoinPath(p.datadir, kCatalogCacheFileName);
    p.park_dir = JoinPath(p.datadir, kParkDirName);
    p.delivery_log = JoinPath(p.datadir, "3DSLink-TitleDelivery.log");
    p.dlp_dir = JoinPath(p.datadir, kDlpDirName);
    p.dlp_child = JoinPath(p.dlp_dir, kDlpChildFileName);
    p.dlp_app_cia = JoinPath(p.dlp_dir, kDlpAppCiaFileName);
    p.dlp_marker = JoinPath(p.dlp_dir, kDlpMarkerFileName);
    return p;
}

std::string ResolvePluginFile(const Paths& paths) {
    if (Exists(paths.plugin_beside)) {
        return paths.plugin_beside;
    }
    // Neither is there: the app's own folder is where the app puts the plugin it carries (plugin_install.hpp), so that is the file in use (with a CIA install, and for a
    // 3dsx in sdmc:/3ds, sdmc:/3ds/3DSLink/AzaharLink.3gx).
    return paths.plugin_in_data;
}

const Paths& CurrentPaths() {
    return g_paths;
}

void SetCurrentPaths(const Paths& paths) {
    g_paths = paths;
}

bool MakeDirs(const std::string& path) {
    std::vector<std::string> chain; // from the folder itself up to (not including) the root
    for (std::string p = StripTrailingSlashes(path); !p.empty(); p = ParentDir(p)) {
        if (p.back() == ':') {
            break; // (the device itself)
        }
        chain.push_back(p);
    }
    for (std::size_t i = chain.size(); i-- > 0;) {
        if (mkdir(chain[i].c_str(), 0777) != 0 && errno != EEXIST) {
            return false;
        }
    }
    return IsDir(StripTrailingSlashes(path)) || path == "sdmc:";
}

bool IsFirstStart(const Paths& paths) {
    return !Exists(paths.config_file);
}

bool Boot9SizeOk(u64 size) {
    return size == 0x10000 || size == 0x8000;
}

std::vector<std::string> Boot9Candidates(const Paths& paths, const std::string& saved_path) {
    std::vector<std::string> list;
    for (const std::string& place : {paths.boot9_beside, paths.boot9_in_data, paths.boot9_godmode, saved_path}) {
        if (place.empty() || std::find(list.begin(), list.end(), place) != list.end()) {
            continue;
        }
        list.push_back(place);
    }
    return list;
}

std::string Boot9Lookup::folder() const {
    return ParentDir(path);
}

Boot9Lookup FindBoot9(const std::vector<std::string>& candidates, const SizeFn& size_of) {
    Boot9Lookup result;
    for (const std::string& candidate : candidates) {
        u64 size = 0;
        if (size_of(candidate, &size) && Boot9SizeOk(size)) {
            result.found = true;
            result.path = candidate;
            return result;
        }
    }
    return result;
}

Boot9Lookup FindBoot9(const Paths& paths, const std::string& saved_path) {
    // (a file that is the lower half of the bootrom has the size of a boot9 and no keys: it does not count)
    Boot9Lookup result = FindBoot9(Boot9Candidates(paths, saved_path), [](const std::string& path, u64* size) {
        return FileSizeOf(path, size) && ClassifyBoot9File(path) != Boot9Verdict::NoKeys;
    });
    if (result.found) {
        return result;
    }
    // No file called boot9.bin: GodMode9 2.x names its dump after the console ("ABC12345678_boot9_00.bin"). The places it looks in by itself are
    // looked into for a name like that.
    for (const std::string& dir : {paths.appdir, paths.datadir, std::string(kGodModeOutDir)}) {
        const std::string hit = FindBoot9InFolder(dir);
        if (!hit.empty()) {
            result.found = true;
            result.path = hit;
            return result;
        }
    }
    return result;
}

bool IsBoot9Name(const std::string& name) {
    std::string lower = name;
    for (char& c : lower) {
        c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    if (lower.size() < 5 || lower.compare(lower.size() - 4, 4, ".bin") != 0) {
        return false;
    }
    return lower.find("boot9") != std::string::npos && lower.find("boot11") == std::string::npos;
}

std::string FindBoot9InFolder(const std::string& dir) {
    DIR* d = opendir(dir.empty() ? "" : (dir.back() == ':' ? dir + "/" : dir).c_str());
    if (!d) {
        return std::string();
    }
    std::string best, best_name;
    int best_rank = 0;
    while (const struct dirent* entry = readdir(d)) {
        const std::string name = entry->d_name;
        std::string lower = name;
        for (char& c : lower) {
            c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }
        if (lower.size() < 5 || lower.compare(lower.size() - 4, 4, ".bin") != 0) {
            continue;
        }
        const std::string path = JoinPath(dir, name);
        u64 size = 0;
        if (!FileSizeOf(path, &size) || !Boot9SizeOk(size)) {
            continue;
        }
        // A boot9 dump is recognised by what is in it, whatever it is called; a file that is not known by its content counts when its name says boot9.
        const Boot9Verdict verdict = ClassifyBoot9File(path);
        if (verdict == Boot9Verdict::NoKeys || (verdict != Boot9Verdict::Good && !IsBoot9Name(name))) {
            continue;
        }
        const int rank = verdict == Boot9Verdict::Good ? 4 : (lower == kBoot9FileName ? 3 : (size == 0x10000 ? 2 : 1));
        if (rank > best_rank || (rank == best_rank && name > best_name)) {
            best_rank = rank;
            best = path;
            best_name = name;
        }
    }
    closedir(d);
    return best;
}

bool IsUsableBoot9(const std::string& file) {
    u64 size = 0;
    return FileSizeOf(file, &size) && Boot9SizeOk(size) && ClassifyBoot9File(file) != Boot9Verdict::NoKeys;
}

namespace {

std::string HexUpper(const u8* data, std::size_t size) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    for (std::size_t i = 0; i < size; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

// The SHA-256 of the bootrom of a retail console (the same on all of them), as dumped by GodMode9: the whole 64 KB, and its upper half (32 KB, where
// the key tables are). The lower half alone is a dump that is no use here.
std::vector<Boot9Fingerprint> g_fingerprints;
const std::vector<Boot9Fingerprint>& Fingerprints() {
    static const std::vector<Boot9Fingerprint> known = {
        {0x10000, "2F88744FEED717856386400A44BBA4B9CA62E76A32C715D4F309C399BF28166F", Boot9Verdict::Good},
        {0x8000, "7331F7EDECE3DD33F2AB4BD0B3A5D607229FD19212C10B734CEDCAF78C1A7B98", Boot9Verdict::Good},
        {0x8000, "DF95ED5B358ED93F2738BAE9E0FC2E4EF1865A62ABA9FCC54A4E0898301F97B4", Boot9Verdict::NoKeys},
    };
    return g_fingerprints.empty() ? known : g_fingerprints;
}

struct Remembered {
    u64 size = 0;
    long mtime = 0;
    Boot9Verdict verdict = Boot9Verdict::Unknown;
};
std::map<std::string, Remembered> g_remembered;

} // namespace

std::string Sha256Hex(const u8* data, std::size_t size) {
    u8 digest[32];
    sha256(data, size, digest);
    return HexUpper(digest, sizeof(digest));
}

void SetBoot9FingerprintsForTest(const std::vector<Boot9Fingerprint>& list) {
    g_fingerprints = list;
    g_remembered.clear();
}

Boot9Verdict ClassifyBoot9File(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode) || !Boot9SizeOk(static_cast<u64>(st.st_size))) {
        return Boot9Verdict::Unknown;
    }
    const auto it = g_remembered.find(path);
    if (it != g_remembered.end() && it->second.size == static_cast<u64>(st.st_size) && it->second.mtime == static_cast<long>(st.st_mtime)) {
        return it->second.verdict;
    }
    Remembered now;
    now.size = static_cast<u64>(st.st_size);
    now.mtime = static_cast<long>(st.st_mtime);
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        sha256_ctx sha;
        sha256_init(&sha);
        u8 buffer[4096];
        u64 left = now.size;
        while (left > 0) {
            const std::size_t got = std::fread(buffer, 1, std::min<u64>(left, sizeof(buffer)), f);
            if (got == 0) {
                break;
            }
            sha256_update(&sha, buffer, got);
            left -= got;
        }
        std::fclose(f);
        if (left == 0) {
            u8 digest[32];
            sha256_final(&sha, digest);
            const std::string hex = HexUpper(digest, sizeof(digest));
            for (const Boot9Fingerprint& fingerprint : Fingerprints()) {
                if (fingerprint.size == now.size && fingerprint.sha256_hex == hex) {
                    now.verdict = fingerprint.verdict;
                }
            }
        }
    }
    g_remembered[path] = now;
    return now.verdict;
}

} // namespace setup
