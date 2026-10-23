#include "search.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>

#include "paths.hpp"

namespace setup {

namespace {

std::string Lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// The path to hand to opendir: the root of a device ("sdmc:") is opened as "sdmc:/".
std::string OpenPath(const std::string& path) {
    return !path.empty() && path.back() == ':' ? path + "/" : path;
}

bool IsDirectoryPath(const std::string& path) {
    struct stat st;
    return stat(OpenPath(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

} // namespace

bool IsTitleIdFolderName(const std::string& name) {
    if (name.size() != 16) {
        return false;
    }
    for (char c : name) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

FileSearch::FileSearch(const std::string& root, const std::string& file_name, Accept accept, const Options& options)
    : file_name_(Lower(file_name)), accept_(std::move(accept)), options_(options) {
    queue_.emplace_back(root, 0);
}

FileSearch::~FileSearch() {
    CloseDir();
}

void FileSearch::CloseDir() {
    if (dir_) {
        closedir(dir_);
        dir_ = nullptr;
    }
}

void FileSearch::Cancel() {
    CloseDir();
    queue_.clear();
    finished_ = true;
}

bool FileSearch::IsFolderEntry(const struct dirent* entry) const {
#ifdef DT_DIR
    if (entry->d_type == DT_DIR) {
        return true;
    }
    if (entry->d_type == DT_REG) {
        return false;
    }
#endif
    return IsDirectoryPath(JoinPath(dir_path_, entry->d_name)); // (this file system does not say: ask)
}

bool FileSearch::Skips(const std::string& parent, const std::string& name) const {
    const std::string lower = Lower(name);
    if (lower == "nintendo 3ds" || lower == "system volume information" || lower == "$recycle.bin") {
        return true;
    }
    const std::string tail = "/luma/plugins";
    const std::string p = Lower(parent);
    const bool in_plugins = p.size() >= tail.size() && p.compare(p.size() - tail.size(), tail.size(), tail) == 0;
    return options_.skip_plugin_title_folders && in_plugins && IsTitleIdFolderName(name);
}

bool FileSearch::Step(int max_entries) {
    while (!finished_ && max_entries > 0) {
        if (!dir_) {
            if (queue_.empty()) {
                finished_ = true;
                break;
            }
            dir_path_ = queue_.front().first;
            dir_depth_ = queue_.front().second;
            queue_.pop_front();
            dir_ = opendir(OpenPath(dir_path_).c_str());
            --max_entries;
            if (!dir_) {
                ++folders_done_; // (a folder that cannot be opened is walked past)
            }
            continue;
        }
        const struct dirent* entry = readdir(dir_);
        if (!entry) {
            CloseDir();
            ++folders_done_;
            continue;
        }
        --max_entries;
        ++entries_seen_;
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        if (IsFolderEntry(entry)) {
            if (dir_depth_ < options_.max_depth && !Skips(dir_path_, name)) {
                queue_.emplace_back(JoinPath(dir_path_, name), dir_depth_ + 1);
            }
        } else if (options_.name_match ? options_.name_match(name) : Lower(name) == file_name_) {
            const std::string path = JoinPath(dir_path_, name);
            if (accept_ && accept_(path)) {
                found_ = true;
                result_ = path;
                CloseDir();
                queue_.clear();
                finished_ = true;
            }
        }
    }
    return finished_;
}

std::vector<std::string> ListSubfolders(const std::string& path, bool* ok) {
    std::vector<std::string> names;
    DIR* dir = opendir(OpenPath(path).c_str());
    if (ok) {
        *ok = dir != nullptr;
    }
    if (!dir) {
        return names;
    }
    while (const struct dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        bool is_dir = false;
#ifdef DT_DIR
        if (entry->d_type == DT_DIR) {
            is_dir = true;
        } else if (entry->d_type == DT_REG) {
            continue;
        } else {
            is_dir = IsDirectoryPath(JoinPath(path, name));
        }
#else
        is_dir = IsDirectoryPath(JoinPath(path, name));
#endif
        if (is_dir) {
            names.push_back(name);
        }
    }
    closedir(dir);
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        const std::string la = Lower(a), lb = Lower(b);
        return la != lb ? la < lb : a < b;
    });
    return names;
}

} // namespace setup
