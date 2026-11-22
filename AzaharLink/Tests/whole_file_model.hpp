// A model of the console's card for azl/whole_file.hpp (F7, 2026-11-21), shared by Tests/whole_file_test.cpp (the order, cut at every step) and
// Tests/settings_test/whole_file_states_test.cpp (the app's own fileio.cpp in front of the states the cuts leave). Every command of the card is
// one step and is done whole or not at all, in order (FAT32 on the SD card, no journal: a damaged FAT is out of reach of any order); the power
// goes before step `budget`, and a write that the cut meets lets `prefix` bytes down. Nothing happens on the card after the cut.
// Three ways of renaming onto a file that exists: Refuse (the console: FSUSER_RenameFile, CTRPF's File::Rename), Replace (Azahar on a Linux
// host: the PC's rename), DeleteThenRename (libctru's rename() when FS says 0x3FC: RenameFile refused, DeleteFile, RenameFile, each a step).
#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "azl/whole_file.hpp"

namespace wfm {

enum class RenameMode { Refuse, Replace, DeleteThenRename };
inline const char* ModeName(RenameMode m) {
    return m == RenameMode::Refuse ? "refuse" : m == RenameMode::Replace ? "replace" : "delete-then-rename";
}
constexpr RenameMode kModes[] = {RenameMode::Refuse, RenameMode::Replace, RenameMode::DeleteThenRename};

using Files = std::map<std::string, std::string>;

struct Card {
    Files files;
    RenameMode mode = RenameMode::Refuse;
    long budget = -1;       // steps that happen before the power goes (-1: it never goes)
    std::size_t prefix = 0; // bytes of the write the cut meets that reach the card
    long fail_step = -1;    // one step that fails alone, the power on (-1: none)
    bool cut = false;
    long steps = 0;         // steps tried, the cut one included
    int calls = 0;          // calls of the Fs interface

    enum class Step { Done, Failed, CutNow, Dead };
    Step Next() {
        if (cut) {
            return Step::Dead;
        }
        if (budget == 0) {
            cut = true;
            ++steps;
            return Step::CutNow;
        }
        if (budget > 0) {
            --budget;
        }
        return steps++ == fail_step ? Step::Failed : Step::Done;
    }
    bool Has(const std::string& p) const { return files.count(p) != 0; }

    // ---- the Fs of azl/whole_file.hpp
    bool Exists(const std::string& p) {
        ++calls;
        return Next() == Step::Done && Has(p);
    }
    void Remove(const std::string& p) {
        ++calls;
        if (Next() == Step::Done) {
            files.erase(p);
        }
    }
    bool Rename(const std::string& from, const std::string& to) {
        ++calls;
        if (Next() != Step::Done || !Has(from)) {
            return false;
        }
        if (Has(to)) {
            if (mode == RenameMode::Refuse) {
                return false;
            }
            if (mode == RenameMode::DeleteThenRename) {
                if (Next() != Step::Done) { // DeleteFile(to)
                    return false;
                }
                files.erase(to);
                if (Next() != Step::Done) { // RenameFile again
                    return false;
                }
            }
        }
        const std::string text = files[from];
        files.erase(from);
        files[to] = text;
        return true;
    }
    // CTRPF's File::Open(RWC | TRUNCATE | SYNC): open (made when missing), SetSize(0), the write (flushed: SYNC), close; then the read back
    bool WriteNew(const std::string& p, const std::string& text) {
        ++calls;
        if (Next() != Step::Done) {
            return false;
        }
        if (!Has(p)) {
            files[p] = "";
        }
        if (Next() != Step::Done) {
            return false;
        }
        files[p].clear();
        const Step w = Next();
        if (w == Step::CutNow) {
            files[p] = text.substr(0, std::min(prefix, text.size()));
            return false;
        }
        if (w != Step::Done) {
            return false;
        }
        files[p] = text;
        if (Next() != Step::Done) { // close
            return false;
        }
        if (Next() != Step::Done) { // the read back
            return false;
        }
        return Has(p) && files[p] == text;
    }
};

// ---- the readers, on a card after a cut (the power back, nothing failing)
struct Read {
    bool present = false;
    std::string text;
    bool operator==(const Read& o) const { return present == o.present && (!present || text == o.text); }
};
inline Read PluginRead(const Files& files, const std::string& path) {
    Card c;
    c.files = files;
    const azl::whole::Source s = azl::whole::Choose(c, path);
    Read r;
    if (s != azl::whole::Source::None) {
        r.present = true;
        r.text = files.at(s == azl::whole::Source::Bak ? azl::whole::BakOf(path) : path);
    }
    return r;
}
// ReadTextFile of ConfigApp/source/fileio.cpp: the file missing and its .bak there, the .bak goes back to its place, then the file is read
inline Read AppRead(Files& files, const std::string& path) {
    const std::string bak = azl::whole::BakOf(path);
    if (!files.count(path) && files.count(bak)) {
        files[path] = files[bak];
        files.erase(bak);
    }
    Read r;
    if (files.count(path)) {
        r.present = true;
        r.text = files[path];
    }
    return r;
}

// ---- the texts and the states a save may meet
const std::string kPath = "sdmc:/3ds/3DSLink/AzaharLink.ini";
const std::string kOld = "enabled=1\nhost=old.example\nport=24872\nnickname=Old\npassword=pw-old\n";
const std::string kNew = "enabled=1\nhost=new.example\nport=24900\nnickname=New\npassword=pw-new\nchat=0\n";
const std::string kNew2 = "enabled=1\nhost=third.example\nport=24901\nnickname=Third\n";
const std::string kStale = "host=stale.example\n";
const std::string kBakOld = "enabled=1\nhost=bak.example\nport=24873\nnickname=Bak\n";
const std::string kTmpWhole = "host=tmp.example\n";
// the boxes of the lists' worker (titles.txt.new, public-lobbies.txt.new): empty is their normal state, and nothing here reads or writes them
const std::string kBoxTitles = "sdmc:/3ds/3DSLink/titles.txt.new";
const std::string kBoxIni = "sdmc:/3ds/3DSLink/AzaharLink.ini.new";

struct State {
    const char* name;
    Files files;
};
inline std::vector<State> States() {
    const std::string tmp = azl::whole::TmpOf(kPath);
    const std::string bak = azl::whole::BakOf(kPath);
    return {
        {"no file", {}},
        {"the old file", {{kPath, kOld}}},
        {"old + a stale .bak", {{kPath, kOld}, {bak, kStale}}},
        {"old + half a .tmp", {{kPath, kOld}, {tmp, "enabled=1\nho"}}},
        {"the .bak alone", {{bak, kBakOld}}},
        {"the .bak + a whole .tmp", {{bak, kBakOld}, {tmp, kTmpWhole}}},
        {"an empty file (1.1.3 cut)", {{kPath, ""}}},
        {"old + empty .new boxes of older builds", {{kPath, kOld}, {kBoxTitles, ""}, {kBoxIni, ""}}},
    };
}

// The prefixes of a cut write: none, one byte, half, all but one, all
inline std::vector<std::size_t> Prefixes(const std::string& text) {
    std::vector<std::size_t> p = {0, 1, text.size() / 2, text.size() - 1, text.size()};
    std::sort(p.begin(), p.end());
    p.erase(std::unique(p.begin(), p.end()), p.end());
    return p;
}

// One Replace on a copy of `files`: the card after it, its answer, the steps it tried
struct Run {
    Files files;
    bool ok = false;
    long steps = 0;
    int calls = 0;
    bool cut = false;
};
inline Run Save(const Files& files, RenameMode mode, long budget, std::size_t prefix, const std::string& text, long fail_step = -1) {
    Card c;
    c.files = files;
    c.mode = mode;
    c.budget = budget;
    c.prefix = prefix;
    c.fail_step = fail_step;
    Run r;
    r.ok = azl::whole::Replace(c, kPath, text);
    r.files = c.files;
    r.steps = c.steps;
    r.calls = c.calls;
    r.cut = c.cut;
    return r;
}

// Every card a cut save can leave: for each state, mode, step of the cut and prefix of the write (`fn(state, mode, files_before, files_after)`)
template <class Fn>
void ForEachCut(Fn fn) {
    for (const State& st : States()) {
        for (RenameMode mode : kModes) {
            const long total = Save(st.files, mode, -1, 0, kNew).steps;
            for (long k = 0; k <= total; ++k) {
                for (std::size_t p : Prefixes(kNew)) {
                    const Run r = Save(st.files, mode, k, p, kNew);
                    fn(st, mode, r);
                }
            }
        }
    }
}

} // namespace wfm
