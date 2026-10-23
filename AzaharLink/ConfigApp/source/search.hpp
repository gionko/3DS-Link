// Looking for a file on the SD card: the search of the whole card the Installation check offers (for AzaharLink.3gx and for boot9.bin), and the
// listing of a folder's sub-folders that the folder browser shows. Plain C++ and POSIX directory calls, so Tests/settings_test/paths_test.cpp
// builds this very file on a PC against a folder called "sdmc:".
#pragma once

#include <dirent.h>

#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace setup {

// A search of a folder tree for a file by name, done a few directory entries at a time so that the screen keeps drawing (and B can cancel).
// It goes through the folders breadth first: a file near the top of the card is found before one deep inside a folder. It never goes into
// the console's own "Nintendo 3DS" folder (encrypted data, thousands of entries, nothing of the user's) nor the leftovers of a PC
// ("System Volume Information", "$RECYCLE.BIN").
class FileSearch {
public:
    // The file at `path` is a hit only when this says so (its size, its header): a file with the right name that is no good is walked past.
    using Accept = std::function<bool(const std::string& path)>;

    struct Options {
        // Do not go into the title folders of sdmc:/luma/plugins (16 hex digits): the copies 3DS Link made there are not where a plugin is looked for.
        bool skip_plugin_title_folders = false;
        int max_depth = 24;
        // When set, a file is a candidate when this says its NAME is good (instead of being the one `file_name` of the constructor): the dump of
        // boot9 that GodMode9 2.x makes is called "<serial>_boot9_00.bin", not "boot9.bin".
        std::function<bool(const std::string& name)> name_match;
    };

    // `root` is "sdmc:" for the whole card; `file_name` is compared without regard to case.
    FileSearch(const std::string& root, const std::string& file_name, Accept accept, const Options& options);
    ~FileSearch();
    FileSearch(const FileSearch&) = delete;
    FileSearch& operator=(const FileSearch&) = delete;

    // Looks at up to `max_entries` more directory entries (opening a folder counts as one). True when the search is over: a hit, the whole tree
    // done, or Cancel().
    bool Step(int max_entries);
    void Cancel();

    bool finished() const {
        return finished_;
    }
    bool found() const {
        return found_;
    }
    const std::string& result() const {
        return result_;
    }
    int folders_done() const {
        return folders_done_;
    }
    long entries_seen() const {
        return entries_seen_;
    }
    // The folder being read (for the progress text).
    const std::string& current_folder() const {
        return dir_path_;
    }

private:
    bool IsFolderEntry(const struct dirent* entry) const;
    bool Skips(const std::string& parent, const std::string& name) const;
    void CloseDir();

    std::string file_name_;
    Accept accept_;
    Options options_;
    std::deque<std::pair<std::string, int>> queue_; // folders still to read, with their depth
    DIR* dir_ = nullptr;
    std::string dir_path_;
    int dir_depth_ = 0;
    bool finished_ = false;
    bool found_ = false;
    std::string result_;
    int folders_done_ = 0;
    long entries_seen_ = 0;
};

// The names of the sub-folders of `path` ("sdmc:" for the card's root), sorted without regard to case. `ok` tells whether the folder could be opened.
std::vector<std::string> ListSubfolders(const std::string& path, bool* ok = nullptr);

// Is `name` the 16 hex digits of a title id?
bool IsTitleIdFolderName(const std::string& name);

} // namespace setup
