#pragma once

#include <string>
#include <algorithm>

namespace ime {

class WubiEncoder {
public:
    static bool IsValidWubiCode(const std::string& code) {
        if (code.empty() || code.length() > 4) return false;
        for (char c : code) {
            if (c < 'a' || c > 'z') return false;
        }
        return true;
    }

    static std::string Normalize(const std::string& input) {
        std::string s = input;
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }
};

} // namespace ime

