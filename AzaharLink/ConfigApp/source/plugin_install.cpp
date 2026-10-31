#include "plugin_install.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "paths.hpp"

namespace setup {

namespace {

constexpr std::size_t kBlock = 64 * 1024; // a read or a write (the buffer is on the heap: the plugin is about 1 MB, the thread's stack is not the place for it)

bool IsFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}

bool IsFolder(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Is the file at `path` exactly `size` bytes `data`? A file that cannot be opened or read to its end is not.
bool FileEqualsBytes(const std::string& path, const std::uint8_t* data, std::size_t size) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    std::vector<std::uint8_t> block(kBlock);
    std::size_t off = 0;
    bool same = true;
    while (same && off < size) {
        const std::size_t want = size - off < block.size() ? size - off : block.size();
        same = std::fread(block.data(), 1, want, f) == want && std::memcmp(block.data(), data + off, want) == 0;
        off += want;
    }
    if (same) {
        same = std::fgetc(f) == EOF && !std::ferror(f); // (nothing after the last byte: a longer file is another file)
    }
    std::fclose(f);
    return same;
}

} // namespace

PluginProbe InspectPluginFile(const std::string& path, const std::uint8_t* data, std::size_t size) {
    PluginProbe probe;
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
        return probe;
    }
    probe.exists = true;
    probe.size = static_cast<std::uint64_t>(st.st_size);
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return probe;
    }
    unsigned char head[12] = {};
    const std::size_t got = std::fread(head, 1, sizeof(head), f);
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (failed) {
        return probe; // (a read that fails is not a file that is the wrong plugin: the card is not saying)
    }
    probe.readable = true;
    probe.header_ok = got == sizeof(head) && std::memcmp(head, "3GX$", 4) == 0;
    if (probe.header_ok) {
        std::memcpy(&probe.version_word, head + 8, 4); // (after the magic and the format number; little endian, as the console is)
    }
    probe.same_bytes = probe.size == size && FileEqualsBytes(path, data, size);
    return probe;
}

WriteResult WritePluginFile(const std::string& dest, const std::uint8_t* data, std::size_t size) {
    const std::string folder = ParentDir(dest);
    if (!folder.empty() && !MakeDirs(folder)) {
        return WriteResult::NoFolder;
    }
    if (IsFolder(dest)) {
        return WriteResult::FolderInTheWay;
    }
    const std::string part = dest + ".part";
    const std::string old = dest + ".old";
    if (!IsFile(dest) && IsFile(old)) {
        std::rename(old.c_str(), dest.c_str()); // (a swap that a power cut left half done: the old file comes back before anything is written)
    }
    std::remove(part.c_str());
    FILE* out = std::fopen(part.c_str(), "wb");
    if (!out) {
        return WriteResult::CannotWrite;
    }
    bool ok = true;
    for (std::size_t off = 0; ok && off < size;) {
        const std::size_t n = size - off < kBlock ? size - off : kBlock;
        ok = std::fwrite(data + off, 1, n, out) == n;
        off += n;
    }
    ok = std::fflush(out) == 0 && ok; // (a card that is full shows here too)
    ok = std::fclose(out) == 0 && ok;
    if (!ok) {
        std::remove(part.c_str());
        return WriteResult::CannotWrite;
    }
    if (!FileEqualsBytes(part, data, size)) {
        std::remove(part.c_str());
        return WriteResult::NotVerified;
    }
    // On the card a rename does not replace a file: the old one is put aside, the new one takes its place, and only then the old one goes (the installer does the same).
    std::remove(old.c_str());
    const bool had_old = std::rename(dest.c_str(), old.c_str()) == 0;
    if (std::rename(part.c_str(), dest.c_str()) != 0) {
        if (had_old) {
            std::rename(old.c_str(), dest.c_str()); // (the old file goes back)
        }
        std::remove(part.c_str());
        return WriteResult::CannotPlace;
    }
    if (had_old) {
        std::remove(old.c_str());
    }
    return WriteResult::Ok;
}

std::string PluginContentStamp(const std::uint8_t* data, std::size_t size) {
    if (!data || size == 0) {
        return std::string();
    }
    std::uint64_t h = 14695981039346656037ull; // FNV-1a, 64 bit
    for (std::size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%llu:h%016llx", static_cast<unsigned long long>(size), static_cast<unsigned long long>(h));
    return buf;
}

PluginEnsure EnsurePluginFile(const std::string& dest, const std::uint8_t* data, std::size_t size, bool usable, std::uint32_t wanted_version_word) {
    PluginEnsure result;
    if (!IsFile(dest) && IsFile(dest + ".old")) {
        std::rename((dest + ".old").c_str(), dest.c_str()); // (see WritePluginFile: the look must see the old file, not nothing)
    }
    PluginProbe probe = InspectPluginFile(dest, data, size);
    result.before = ClassifyPluginCard(probe, wanted_version_word);
    result.step = DecideInstall(result.before, usable);
    if (result.step != PluginStep::Leave) {
        result.write = WritePluginFile(dest, data, size);
        probe = InspectPluginFile(dest, data, size); // (the card as it is now: the one reading of the file that is not the first)
    }
    result.after = ClassifyPluginCard(probe, wanted_version_word);
    result.header_ok = probe.header_ok;
    result.version_word = probe.version_word;
    result.size = probe.size;
    if (result.step != PluginStep::Leave && result.write == WriteResult::Ok && result.after != PluginCard::Same) {
        result.write = WriteResult::NotVerified; // (written, and the card does not hold it: say so, never "put")
    }
    if (result.after == PluginCard::Same && IsFile(dest + ".old")) {
        std::remove((dest + ".old").c_str()); // (a power cut between the rename of the new file and the removal of the old one leaves a megabyte behind: the card holds the plugin, the old file is not needed)
    }
    return result;
}

} // namespace setup
