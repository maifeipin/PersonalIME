#pragma once

#include <string>
#include <algorithm>

namespace ime {

class WubiEncoder {
public:
    // Standard 86 Wubi code rule for built-in dictionary & decomposition (1..4 letters a-y)
    static bool IsValidWubiCode(const std::string& code) {
        if (code.empty() || code.length() > 4) return false;
        for (char c : code) {
            if (c < 'a' || c > 'y') return false;
        }
        return true;
    }

    // Extended Wubi code rule for user custom entries & queries (1..64 letters a-y)
    static bool IsValidCustomWubiCode(const std::string& code) {
        if (code.empty() || code.length() > 64) return false;
        for (char c : code) {
            char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower < 'a' || lower > 'y') return false;
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

