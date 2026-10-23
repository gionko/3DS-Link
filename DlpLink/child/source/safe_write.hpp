// How the installer puts a file of its package on the SD card, and how it tells that the file already there is the right one: the plugin and the copy of the
// app's CIA (sdmc:/3ds/3DSLink/dlp/3dslink.cia, which the app composes the Download Play child from: DlpLink/docs/COMPOSITORE.md) go the same way. Plain stdio and
// stat with no libctru in it, so DlpLink/tests/child_logic_test.cpp builds this very file on a PC and runs it on a temp folder (the log, the progress and the
// "the system is closing" flag are three small hooks that main.cpp fills from its job record).
#pragma once

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace inst {

constexpr std::size_t kBlock = 256 * 1024; // the size of a read or a write, and of the buffer the callers lend

// What the file code asks of its surroundings. (A plain interface: the 3DS build has no RTTI and no exceptions.)
class Hooks {
public:
    virtual void Log(const char* line) = 0;                 // one line for the installer's log
    virtual void Advance(std::uint64_t bytes) = 0;          // `bytes` more of the work are done (the progress bar)
    virtual bool Cancelled() = 0;                           // the system is closing the app
    virtual ~Hooks() = default;
};

inline void Logf(Hooks& h, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
inline void Logf(Hooks& h, const char* fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    h.Log(buf);
}

inline bool FileSize(FILE* f, std::uint64_t* size) {
    if (fseek(f, 0, SEEK_END) != 0) {
        return false;
    }
    const long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        return false;
    }
    *size = static_cast<std::uint64_t>(n);
    return true;
}

// Is the file at `path` the same bytes as the (open) file `ref` of `size` bytes (the plugin, the app's CIA of the RomFS)? Both are read from the start, half of the
// buffer `buf` (`buf_size` bytes, at least 2) for each. A file that is not there, or is another size, is "not the same". `ref` is left anywhere.
inline bool SameFileAs(FILE* ref, const char* path, std::uint64_t size, std::uint8_t* buf, std::size_t buf_size) {
    FILE* g = fopen(path, "rb");
    if (!g) {
        return false;
    }
    std::uint64_t other = 0;
    bool same = FileSize(g, &other) && other == size && fseek(ref, 0, SEEK_SET) == 0;
    const std::size_t half = buf_size / 2;
    std::uint8_t* const second = buf + half;
    std::uint64_t off = 0;
    while (same && off < size) {
        const std::size_t want = size - off < half ? static_cast<std::size_t>(size - off) : half;
        const std::size_t a = fread(buf, 1, want, ref);
        const std::size_t b = fread(second, 1, want, g);
        same = a == want && b == want && std::memcmp(buf, second, want) == 0;
        off += want;
    }
    fclose(g);
    return same;
}

// A file the installer puts on the SD card whole.
struct Target {
    const char* what;     // for the log
    const char* path;     // where it ends up
    const char* tmp;      // written whole here first (the ".part")
    const char* bak;      // what was at `path`, for the few moments of the swap
    bool stop_on_cancel;  // a close request between two blocks leaves the old file as it is (the copy; the plugin is part of the work the questions promised)
};

// Writes `size` bytes of `in` through a temp file and renames: the new file is written whole next to the old one, the old one is put aside (whether a rename onto an
// existing file fails or replaces it depends on the layer under newlib; this order does not need either), the new one takes its place and only then the old one goes.
// A failure puts the old one back; a power cut between the two renames leaves the .old file, which the next run puts back before anything else. So the file on the
// card is whole, or it is the old one. Every failure is in the log with its errno. `buf` (`buf_size` bytes) is the block buffer.
inline bool WriteWhole(FILE* in, std::uint64_t size, const Target& t, std::uint8_t* buf, std::size_t buf_size, Hooks& h) {
    remove(t.tmp);
    struct stat sb;
    if (stat(t.path, &sb) != 0 && stat(t.bak, &sb) == 0) {
        rename(t.bak, t.path); // an interrupted swap of an earlier run
    }
    FILE* out = fopen(t.tmp, "wb");
    if (!out) {
        Logf(h, "fopen(%s) failed, errno %d", t.tmp, errno);
        return false;
    }
    bool ok = fseek(in, 0, SEEK_SET) == 0;
    std::uint64_t off = 0;
    while (ok && off < size) {
        if (t.stop_on_cancel && h.Cancelled()) {
            Logf(h, "%s copy stopped at offset %llu: the system is closing the app", t.what, static_cast<unsigned long long>(off));
            ok = false;
            break;
        }
        const std::size_t want = size - off < buf_size ? static_cast<std::size_t>(size - off) : buf_size;
        const std::size_t n = fread(buf, 1, want, in);
        if (n != want || fwrite(buf, 1, n, out) != n) {
            Logf(h, "%s copy failed at offset %llu: read %zu of %zu, errno %d", t.what, static_cast<unsigned long long>(off), n, want, errno);
            ok = false;
            break;
        }
        off += n;
        h.Advance(n);
    }
    const bool flushed = fflush(out) == 0; // a full card shows here too
    const int flush_errno = errno;
    const bool closed = fclose(out) == 0;
    if (ok && (!flushed || !closed)) {
        Logf(h, "%s copy: %s failed, errno %d", t.what, flushed ? "close" : "flush", flushed ? errno : flush_errno);
        ok = false;
    }
    if (ok) {
        remove(t.bak);
        const bool had_old = rename(t.path, t.bak) == 0;
        ok = rename(t.tmp, t.path) == 0;
        if (!ok) {
            Logf(h, "rename(%s) failed, errno %d", t.tmp, errno);
            if (had_old) {
                rename(t.bak, t.path); // the old file goes back
            }
        } else if (had_old) {
            remove(t.bak);
        }
    }
    if (!ok) {
        remove(t.tmp);
    }
    return ok;
}

// The copy of the app's CIA for the app: the file `src_path` (romfs:/3dslink.cia, `size` bytes) written into `dir` as `t`, through a .part file and a rename, then read
// back against the source from its final place (a copy that does not match is taken away: the app would refuse it anyway, and a missing file is written again next
// time). Nothing is started when the system is already closing the app. False on any failure, logged with its errno; the caller treats it as a courtesy that failed,
// never as a failure of the job.
inline bool SaveCopy(const char* src_path, const char* dir, std::uint64_t size, const Target& t, std::uint8_t* buf, std::size_t buf_size, Hooks& h) {
    if (h.Cancelled()) {
        Logf(h, "%s: not started, the system is closing the app", t.what);
        return false;
    }
    mkdir(dir, 0777);
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        Logf(h, "%s: %s cannot be made, errno %d", t.what, dir, errno);
        return false;
    }
    FILE* in = fopen(src_path, "rb");
    if (!in) {
        Logf(h, "%s: %s: fopen failed, errno %d", t.what, src_path, errno);
        return false;
    }
    bool ok = WriteWhole(in, size, t, buf, buf_size, h);
    if (ok && !SameFileAs(in, t.path, size, buf, buf_size)) {
        Logf(h, "%s: %s does not match the package after the write", t.what, t.path);
        remove(t.path);
        ok = false;
    }
    fclose(in);
    return ok;
}

} // namespace inst
