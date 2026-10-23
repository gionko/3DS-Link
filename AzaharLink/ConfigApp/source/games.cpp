#include "games.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "settings.hpp"

namespace setup {

namespace {

bool IsDirectory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// True and the size when `path` is a regular file.
bool FileSize(const std::string& path, u64* size) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
        return false;
    }
    if (size) {
        *size = static_cast<u64>(st.st_size);
    }
    return true;
}

char Lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoreCase(const std::string& a, const char* b) {
    const std::size_t n = std::strlen(b);
    if (a.size() != n) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (Lower(a[i]) != Lower(b[i])) {
            return false;
        }
    }
    return true;
}

bool Ends3gx(const std::string& name) {
    return name.size() > 4 && name[name.size() - 4] == '.' && Lower(name[name.size() - 3]) == '3' &&
           Lower(name[name.size() - 2]) == 'g' && Lower(name[name.size() - 1]) == 'x';
}

// UTF-16 little endian (up to `units` of them, ended by a zero) to UTF-8. Line breaks and other control characters become
// spaces (the title of a game can be on two lines), a lone surrogate becomes U+FFFD.
std::string Utf16ToUtf8(const u8* p, int units) {
    std::string out;
    for (int i = 0; i < units; ++i) {
        u32 cp = static_cast<u32>(p[2 * i]) | (static_cast<u32>(p[2 * i + 1]) << 8);
        if (cp == 0) {
            break;
        }
        if (cp >= 0xD800 && cp < 0xDC00) {
            const u32 lo = i + 1 < units ? static_cast<u32>(p[2 * i + 2]) | (static_cast<u32>(p[2 * i + 3]) << 8) : 0;
            if (lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xDC00 && cp < 0xE000) {
            cp = 0xFFFD;
        }
        if (cp < 0x20 || cp == 0x7F) {
            cp = ' ';
        }
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    // Trim, and make a run of spaces one.
    std::string clean;
    for (char c : out) {
        if (c == ' ' && (clean.empty() || clean.back() == ' ')) {
            continue;
        }
        clean += c;
    }
    while (!clean.empty() && clean.back() == ' ') {
        clean.pop_back();
    }
    return clean;
}

// The sort key of a name: code points, lower case, the accented letters of Latin-1 as the plain letter (the same table as
// TitleShare's catalog, so the two lists agree).
std::vector<u32> SortKey(const std::string& utf8) {
    static const char* const kPlain = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty"; // U+00C0..U+00FF
    std::vector<u32> key;
    for (std::size_t i = 0; i < utf8.size();) {
        const u8 c = static_cast<u8>(utf8[i]);
        u32 cp = c;
        int extra = 0;
        if (c >= 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else if (c >= 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if (c >= 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        }
        ++i;
        for (int k = 0; k < extra && i < utf8.size() && (static_cast<u8>(utf8[i]) & 0xC0) == 0x80; ++k, ++i) {
            cp = (cp << 6) | (static_cast<u8>(utf8[i]) & 0x3F);
        }
        if (cp >= 0xC0 && cp <= 0xFF) {
            cp = static_cast<u8>(kPlain[cp - 0xC0]);
        }
        if (cp < 0x80) {
            cp = static_cast<u32>(Lower(static_cast<char>(cp)));
        }
        key.push_back(cp);
    }
    return key;
}

} // namespace

std::string TidText(u64 tid) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%08lX%08lX", static_cast<unsigned long>(tid >> 32), static_cast<unsigned long>(tid & 0xFFFFFFFFu));
    return buf;
}

bool IsApplicationTid(u64 tid) {
    return static_cast<u32>(tid >> 32) == 0x00040000u;
}

std::string GamePluginDir(u64 tid) {
    return std::string(kPluginsDir) + "/" + TidText(tid);
}

std::string GamePluginPath(u64 tid) {
    return GamePluginDir(tid) + "/" + kPluginFileName;
}

bool ParseSmdh(const u8* data, std::size_t size, int language, GameInfo& out) {
    if (!data || size < kSmdhSize || std::memcmp(data, "SMDH", 4) != 0) {
        return false;
    }
    // The console's language first, then English, Japanese, French, German, Italian, Spanish, Dutch, Portuguese (the order
    // TitleShare's catalog uses).
    static const int kFallback[] = {1, 0, 2, 3, 4, 5, 8, 9};
    int order[1 + sizeof(kFallback) / sizeof(kFallback[0])];
    order[0] = language >= 0 && language < 16 ? language : 1;
    for (std::size_t i = 0; i < sizeof(kFallback) / sizeof(kFallback[0]); ++i) {
        order[1 + i] = kFallback[i];
    }
    out.name.clear();
    for (int index : order) {
        // 16 titles of 0x200 bytes from offset 8: the short title (0x80 bytes), the long one (0x100), the publisher (0x80). The list shows
        // the long one ("The Legend of Zelda: Tri Force Heroes"), the whole name; a title that has only a short one gives that.
        const u8* entry = data + 8 + 0x200 * index;
        out.name = Utf16ToUtf8(entry + 0x80, 0x80);
        if (out.name.empty()) {
            out.name = Utf16ToUtf8(entry, 0x40);
        }
        if (!out.name.empty()) {
            break;
        }
    }
    if (out.name.empty()) {
        return false;
    }
    std::memcpy(out.icon, data + 0x24C0, sizeof(out.icon)); // (the small icon, 24x24 at 0x2040, is not kept: the large one is drawn at half size)
    return true;
}

bool NameLess(const std::string& a, const std::string& b) {
    return SortKey(a) < SortKey(b);
}

void LayoutIcon(const u16* tiled, int size, int pot, u16* out) {
    std::memset(out, 0, sizeof(u16) * static_cast<std::size_t>(pot) * static_cast<std::size_t>(pot));
    // A row of tiles of the file is size/8 tiles of 64 pixels, a row of tiles of the texture pot/8 of them: each row of the file
    // goes to the start of a row of the texture, from the first one; the rows after the last stay empty. (The first row of the
    // texture is the top of the picture: the sub-texture {0, 1, size/pot, 1 - size/pot} shows exactly the rows of the icon.)
    u16* dest = out;
    const u16* src = tiled;
    for (int row = 0; row < size; row += 8) {
        std::memcpy(dest, src, sizeof(u16) * static_cast<std::size_t>(size) * 8);
        src += static_cast<std::size_t>(size) * 8;
        dest += static_cast<std::size_t>(pot) * 8;
    }
}

namespace {

constexpr u32 kCacheMagic = 0x3143474Cu; // "LGC1"
constexpr u32 kCacheFormat = 2; // (2: the names are the long titles, 392 bytes a name; a cache of format 1 is read again)
constexpr std::size_t kCacheRecordBytes = 8 + 2 + kCachedNameBytes + sizeof(u16) * kLargeIcon * kLargeIcon;
constexpr u32 kCacheMaxGames = 4096;

u32 Fnv(u32 h, const u8* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

void PutU32(u8* p, u32 v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<u8>(v >> (8 * i));
    }
}

u32 GetU32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

// A record: tid (8 bytes, little endian), version (2), name (kCachedNameBytes, NUL padded), icon (the 2304 values, little endian).
// Bytes only, so the file is the same on the console and on a PC.
void EncodeRecord(const GameInfo& game, u8* out) {
    std::memset(out, 0, kCacheRecordBytes);
    for (int k = 0; k < 8; ++k) {
        out[k] = static_cast<u8>(game.tid >> (8 * k));
    }
    out[8] = static_cast<u8>(game.version);
    out[9] = static_cast<u8>(game.version >> 8);
    std::memcpy(out + 10, game.name.data(), std::min(game.name.size(), kCachedNameBytes - 1));
    u8* icon = out + 10 + kCachedNameBytes;
    for (int i = 0; i < kLargeIcon * kLargeIcon; ++i) {
        icon[2 * i] = static_cast<u8>(game.icon[i]);
        icon[2 * i + 1] = static_cast<u8>(game.icon[i] >> 8);
    }
}

void DecodeRecord(const u8* in, GameInfo& game) {
    game.tid = 0;
    for (int k = 7; k >= 0; --k) {
        game.tid = (game.tid << 8) | in[k];
    }
    game.version = static_cast<u16>(in[8] | (in[9] << 8));
    char name[kCachedNameBytes];
    std::memcpy(name, in + 10, kCachedNameBytes);
    name[kCachedNameBytes - 1] = '\0';
    game.name = name;
    const u8* icon = in + 10 + kCachedNameBytes;
    for (int i = 0; i < kLargeIcon * kLargeIcon; ++i) {
        game.icon[i] = static_cast<u16>(icon[2 * i] | (icon[2 * i + 1] << 8));
    }
}

} // namespace

bool LoadGameCache(const char* path, int language, std::vector<GameInfo>& out) {
    out.clear();
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        return false;
    }
    u8 header[20];
    bool ok = std::fread(header, 1, sizeof(header), f) == sizeof(header) && GetU32(header) == kCacheMagic &&
              GetU32(header + 4) == kCacheFormat && GetU32(header + 8) == static_cast<u32>(language) &&
              GetU32(header + 12) <= kCacheMaxGames;
    const u32 count = ok ? GetU32(header + 12) : 0;
    // The file must be exactly as long as its header says before anything is set aside for it: a damaged count would ask for memory the console has not got.
    struct stat st;
    ok = ok && fstat(fileno(f), &st) == 0 && static_cast<u64>(st.st_size) == sizeof(header) + static_cast<u64>(count) * kCacheRecordBytes;
    std::vector<u8> records(ok ? static_cast<std::size_t>(count) * kCacheRecordBytes : 0);
    ok = ok && (records.empty() || std::fread(records.data(), 1, records.size(), f) == records.size());
    // Nothing may follow the records: a longer file is not the one that was written.
    u8 extra = 0;
    ok = ok && std::fread(&extra, 1, 1, f) == 0;
    std::fclose(f);
    ok = ok && Fnv(2166136261u, records.data(), records.size()) == GetU32(header + 16);
    if (!ok) {
        return false;
    }
    out.resize(count);
    for (u32 i = 0; i < count; ++i) {
        DecodeRecord(records.data() + static_cast<std::size_t>(i) * kCacheRecordBytes, out[i]);
    }
    return true;
}

bool SaveGameCache(const char* path, int language, const std::vector<GameInfo>& games) {
    if (games.size() > kCacheMaxGames) {
        return false;
    }
    std::vector<u8> records(games.size() * kCacheRecordBytes);
    for (std::size_t i = 0; i < games.size(); ++i) {
        EncodeRecord(games[i], records.data() + i * kCacheRecordBytes);
    }
    u8 header[20];
    PutU32(header, kCacheMagic);
    PutU32(header + 4, kCacheFormat);
    PutU32(header + 8, static_cast<u32>(language));
    PutU32(header + 12, static_cast<u32>(games.size()));
    PutU32(header + 16, Fnv(2166136261u, records.data(), records.size()));
    const std::string tmp = std::string(path) + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        return false;
    }
    bool ok = std::fwrite(header, 1, sizeof(header), f) == sizeof(header) &&
              (records.empty() || std::fwrite(records.data(), 1, records.size(), f) == records.size());
    ok = std::fclose(f) == 0 && ok;
    if (!ok) {
        std::remove(tmp.c_str());
        return false;
    }
    std::remove(path); // on the card a rename does not replace
    if (std::rename(tmp.c_str(), path) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

Slot ClassifyFolder(const std::vector<std::string>& file_names) {
    bool ours = false;
    bool other = false;
    for (const std::string& name : file_names) {
        if (!Ends3gx(name)) {
            continue;
        }
        if (EqualsIgnoreCase(name, kPluginFileName)) {
            ours = true;
        } else {
            other = true;
        }
    }
    if (other) {
        return Slot::Other;
    }
    return ours ? Slot::On : Slot::Off;
}

SlotInfo ReadSlot(u64 tid) {
    SlotInfo info;
    const std::string dir = GamePluginDir(tid);
    DIR* d = opendir(dir.c_str());
    if (!d) {
        return info;
    }
    std::vector<std::string> files;
    u64 copy_size = 0;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        const std::string name = entry->d_name;
        u64 size = 0;
        if (name == "." || name == ".." || !Ends3gx(name) || !FileSize(dir + "/" + name, &size)) {
            continue; // (a sub-folder that happens to end in .3gx is not a plugin)
        }
        files.push_back(name);
        if (EqualsIgnoreCase(name, kPluginFileName)) {
            copy_size = size;
        }
    }
    closedir(d);
    info.slot = ClassifyFolder(files);
    info.copy_size = copy_size;
    return info;
}

std::set<std::string> ListPluginFolders() {
    std::set<std::string> folders;
    DIR* d = opendir(kPluginsDir);
    if (!d) {
        return folders;
    }
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() != 16 || !IsDirectory(std::string(kPluginsDir) + "/" + name)) {
            continue;
        }
        for (char& c : name) {
            c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
        }
        folders.insert(name);
    }
    closedir(d);
    return folders;
}

bool CopyPluginFile(const char* src, const char* dst) {
    FILE* in = std::fopen(src, "rb");
    if (!in) {
        return false;
    }
    const std::string part = std::string(dst) + ".part";
    FILE* out = std::fopen(part.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        return false;
    }
    std::vector<char> buffer(64 * 1024); // (not a static one: the copies of the titles are made by a thread of their own, one may run beside another)
    u64 total = 0;
    bool ok = true;
    std::size_t got;
    while (ok && (got = std::fread(buffer.data(), 1, buffer.size(), in)) > 0) {
        ok = std::fwrite(buffer.data(), 1, got, out) == got;
        total += got;
    }
    ok = ok && !std::ferror(in);
    std::fclose(in);
    const bool closed = std::fclose(out) == 0;
    u64 written = 0;
    if (!ok || !closed || total == 0 || !FileSize(part, &written) || written != total) {
        std::remove(part.c_str());
        return false;
    }
    std::remove(dst); // on the card a rename does not replace
    if (std::rename(part.c_str(), dst) != 0) {
        std::remove(part.c_str());
        return false;
    }
    return true;
}

EnableResult EnableGame(u64 tid, const char* source) {
    if (ReadSlot(tid).slot == Slot::Other) {
        return EnableResult::OtherPlugin;
    }
    if (!FileSize(source, nullptr)) {
        return EnableResult::SourceMissing;
    }
    mkdir("sdmc:/luma", 0777);
    mkdir(kPluginsDir, 0777);
    const std::string dir = GamePluginDir(tid);
    if (mkdir(dir.c_str(), 0777) != 0 && errno != EEXIST) {
        return EnableResult::CopyFailed;
    }
    return CopyPluginFile(source, GamePluginPath(tid).c_str()) ? EnableResult::Ok : EnableResult::CopyFailed;
}

bool FilesDiffer(const char* a, const char* b) {
    FILE* fa = std::fopen(a, "rb");
    if (!fa) {
        return false;
    }
    FILE* fb = std::fopen(b, "rb");
    if (!fb) {
        std::fclose(fa);
        return false;
    }
    std::vector<char> ba(16 * 1024), bb(16 * 1024); // (not static: the check runs in the thread of the copies)
    bool differ = false;
    for (;;) {
        const std::size_t ga = std::fread(ba.data(), 1, ba.size(), fa);
        const std::size_t gb = std::fread(bb.data(), 1, bb.size(), fb);
        if (std::ferror(fa) || std::ferror(fb)) {
            break; // (a read error is not a difference)
        }
        if (ga != gb || (ga > 0 && std::memcmp(ba.data(), bb.data(), ga) != 0)) {
            differ = true;
            break;
        }
        if (ga == 0) {
            break;
        }
    }
    std::fclose(fa);
    std::fclose(fb);
    return differ;
}

EnableResult RefreshGameCopy(u64 tid, const char* source, bool* copied) {
    if (copied) {
        *copied = false;
    }
    if (ReadSlot(tid).slot != Slot::On) {
        return EnableResult::Ok; // (no copy of ours here: nothing to bring up to date)
    }
    u64 want = 0;
    if (!FileSize(source, &want)) {
        return EnableResult::SourceMissing;
    }
    u64 have = 0;
    const bool same_size = FileSize(GamePluginPath(tid), &have) && have == want; // (another size is stale without reading a byte of either)
    if (same_size && !FilesDiffer(GamePluginPath(tid).c_str(), source)) {
        return EnableResult::Ok;
    }
    const EnableResult result = EnableGame(tid, source);
    if (copied) {
        *copied = result == EnableResult::Ok;
    }
    return result;
}

bool DisableGame(u64 tid) {
    const std::string path = GamePluginPath(tid);
    std::remove(path.c_str());
    std::remove((path + ".part").c_str()); // (what an interrupted copy leaves)
    const bool gone = !FileSize(path, nullptr);
    rmdir(GamePluginDir(tid).c_str()); // only an empty folder goes: another file in it is never touched
    return gone;
}

} // namespace setup
