// A small file written whole or not at all (F7, 2026-11-21): the one order of the app (ConfigApp/source/fileio.cpp, WriteTextFile and
// ReadTextFile) written once for the plugin (Plugin::SaveConfig / LoadConfig, AzaharLink.ini). Pure: the card is the caller's `Fs`, so
// Tests/whole_file_test.cpp cuts the power at every step of it on the PC, and Tests/settings_test/whole_file_states_test.cpp holds the app's
// own fileio.cpp to the same states.
//
// Why: until 1.1.4 the plugin opened AzaharLink.ini with TRUNCATE (CTRPF's open of a file: the open, then FSFILE_SetSize(0), then the
// write; this header touches no file itself): a cut between the two left a file of 0 bytes, the plugin then took the defaults and the app, seeing no key, wrote the defaults
// over it - room, password and nickname lost. Now the new text is finished in <path>.tmp first; the old file goes aside as <path>.bak (a
// rename on the card never replaces: the stale .bak goes first), the .tmp takes its place, the .bak goes (F7-rev: always, an old file or not). A cut at any step leaves a
// whole file: <path> (the old or the new one), or <path>.bak alone - and the readers take <path>.bak when <path> is missing.
#pragma once

#include <string>

namespace azl {
namespace whole {

constexpr const char* kTmpSuffix = ".tmp";
constexpr const char* kBakSuffix = ".bak";
inline std::string TmpOf(const std::string& path) { return path + kTmpSuffix; }
inline std::string BakOf(const std::string& path) { return path + kBakSuffix; }

enum class Source { Path, Bak, None };

// Fs: bool Exists(path); bool WriteNew(path, text) (made empty, written, closed - and read back where the card allows it: true only when
// the whole text is there); bool Rename(from, to) (it may refuse a destination that exists: the order never asks for one); void Remove(path).
template <class Fs>
bool Replace(Fs& fs, const std::string& path, const std::string& text) {
    const std::string tmp = TmpOf(path);
    const std::string bak = BakOf(path);
    if (!fs.WriteNew(tmp, text)) {
        fs.Remove(tmp);
        return false;
    }
    const bool had_old = fs.Exists(path);
    if (had_old) {
        fs.Remove(bak);
        if (!fs.Rename(path, bak)) {
            fs.Remove(tmp);
            return false;
        }
    }
    if (!fs.Rename(tmp, path)) {
        if (had_old) {
            fs.Rename(bak, path);
        }
        fs.Remove(tmp);
        return false;
    }
    // F7-rev: the .bak goes whether or not this save found an old file: a save with no file at its place met either nothing or a .bak that a
    // cut save left alone (LoadConfig reads it, ReadTextFile puts it back) - the new file is whole at its place now, and a .bak left next to it
    // would be a stale copy, one save behind, for a reader that misses the file
    fs.Remove(bak);
    return true;
}

// Which file holds the settings: <path> when it is there (an empty one too, as ReadTextFile), else <path>.bak (a write cut between its two
// renames). A .bak next to <path> is a stale one (a save cut at its very last step): never read while <path> answers.
template <class Fs>
Source Choose(Fs& fs, const std::string& path) {
    if (fs.Exists(path)) {
        return Source::Path;
    }
    if (fs.Exists(BakOf(path))) {
        // F7-rev: the file looked at once more before its .bak is taken: CTRPF's File::Exists answers 0 whenever its look at the card fails,
        // a passing error of the card too (File.o of 0.8.0), where the app's reader takes its .bak only on ENOENT
        return fs.Exists(path) ? Source::Path : Source::Bak;
    }
    return Source::None;
}

} // namespace whole
} // namespace azl
