// Word wrap for the messages shown on the screen.
#pragma once

#include <string>
#include <vector>

namespace azl {

/// Splits `text` into lines of at most `max_chars` characters, breaking at spaces (a word longer than a line is
/// cut, a '\n' ends a line). The overlay of the framework is one line of the top screen wide (about 60 characters
/// of its font); a longer message would be cut off at the edge and the end of the reason with it.
inline std::vector<std::string> WrapLines(const std::string& text, std::size_t max_chars) {
    std::vector<std::string> lines;
    if (max_chars == 0) {
        return lines;
    }
    std::string line;
    const auto end_line = [&]() {
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
        line.clear();
    };
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t stop = pos;
        while (stop < text.size() && text[stop] != ' ' && text[stop] != '\n') {
            ++stop;
        }
        std::string word = text.substr(pos, stop - pos);
        while (word.size() > max_chars) { // a word that never fits: cut it
            end_line();
            lines.push_back(word.substr(0, max_chars));
            word.erase(0, max_chars);
        }
        if (!word.empty()) {
            if (!line.empty() && line.size() + 1 + word.size() > max_chars) {
                end_line();
            }
            line += (line.empty() ? "" : " ") + word;
        }
        if (stop < text.size() && text[stop] == '\n') {
            end_line();
        }
        pos = stop + 1;
    }
    end_line();
    return lines;
}

} // namespace azl
