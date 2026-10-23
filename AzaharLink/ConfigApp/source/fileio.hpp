// Reading and writing the small text files of the app (AzaharLink.ini, the list of rooms, 3DSLink.cfg) one careful way for all of them.
#pragma once

#include <cstddef>
#include <string>

namespace setup {

// What reading a file said. "Missing" means there is no such file: any other failure to read it (the card is busy, a read error half way) is
// Unreadable, and a file that could not be read is never written over with the defaults.
enum class LoadResult { Loaded, Missing, Unreadable };

// Reads at most about `max_bytes` of the file (it stops after the chunk that reaches them). `content` is empty unless the result is Loaded.
LoadResult ReadTextFile(const std::string& path, std::size_t max_bytes, std::string& content);

// Writes the file by finishing "<path>.tmp" first and only then putting it in its place, so that a card that is full or pulled in the middle
// leaves the old file whole. False when it could not be written (the old file is then still there).
bool WriteTextFile(const std::string& path, const std::string& content);

} // namespace setup
