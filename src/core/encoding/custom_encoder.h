#pragma once

#include <string>
#include <algorithm>

namespace ime {

class CustomEncoder {
public:
    static bool IsShortcutCode(const std::string& code) {
        return !code.empty() && code[0] == ';';
    }

    static std::string Normalize(const std::string& input) {
        std::string s = input;
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }
};

} // namespace ime

