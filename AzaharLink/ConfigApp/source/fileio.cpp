#include "fileio.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>

namespace setup {

namespace {

bool IsFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

} // namespace

LoadResult ReadTextFile(const std::string& path, std::size_t max_bytes, std::string& content) {
    content.clear();
    const std::string previous = path + ".bak";
    FILE* f = std::fopen(path.c_str(), "rb");
    int error = f ? 0 : errno;
    if (!f && error == ENOENT && std::rename(previous.c_str(), path.c_str()) == 0) {
        // A write that was cut off between taking the old file aside and putting the new one in its place: the old file is whole, so it is the file.
        f = std::fopen(path.c_str(), "rb");
        error = f ? 0 : errno;
    }
    if (!f) {
        return error == ENOENT ? LoadResult::Missing : LoadResult::Unreadable;
    }
    char chunk[512];
    std::size_t n;
    while (content.size() < max_bytes && (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        content.append(chunk, n);
    }
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (failed) {
        content.clear();
        return LoadResult::Unreadable;
    }
    return LoadResult::Loaded;
}

bool WriteTextFile(const std::string& path, const std::string& content) {
    const std::string temporary = path + ".tmp";
    const std::string previous = path + ".bak";
    FILE* f = std::fopen(temporary.c_str(), "wb");
    if (!f) {
        return false;
    }
    bool ok = std::fwrite(content.data(), 1, content.size(), f) == content.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok) {
        std::remove(temporary.c_str());
        return false;
    }
    // On the card a rename does not replace: the old file is taken aside, the new one put in its place, and the old one dropped. A cut at any
    // moment leaves a whole file (ReadTextFile puts the old one back when it is the only one left).
    const bool had_old = IsFile(path);
    if (had_old) {
        std::remove(previous.c_str());
        if (std::rename(path.c_str(), previous.c_str()) != 0) {
            std::remove(temporary.c_str());
            return false;
        }
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        if (had_old) {
            std::rename(previous.c_str(), path.c_str());
        }
        std::remove(temporary.c_str());
        return false;
    }
    if (had_old) {
        std::remove(previous.c_str());
    }
    return true;
}

} // namespace setup
