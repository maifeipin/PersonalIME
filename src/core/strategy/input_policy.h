#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>
#include "../types.h"
#include "../encoding/shuangpin_decoder.h"

namespace ime {

// Well-known dictionary IDs
inline constexpr const char* DICT_ID_CUSTOM = "dict_custom";
inline constexpr const char* DICT_ID_BUILTIN_WUBI = "dict_builtin_wubi";
inline constexpr const char* DICT_ID_BUILTIN_PINYIN = "dict_builtin_pinyin";

struct InputPolicy {
    uint64_t revision = 1;

    // 1. Input Methods (输入方式)
    InputScheme primary_method = InputScheme::Wubi86;
    bool mixed_input_enabled = false;
    std::vector<InputScheme> input_method_order = {
        InputScheme::Wubi86,
        InputScheme::Pinyin,
        InputScheme::Shuangpin
    };
    struct MethodConfig {
        InputScheme scheme = InputScheme::Wubi86;
        bool enabled = true;
    };
    std::vector<MethodConfig> method_configs = {
        {InputScheme::Wubi86, true},
        {InputScheme::Pinyin, true},
        {InputScheme::Shuangpin, true}
    };
    ShuangpinLayout shuangpin_layout = ShuangpinLayout::Natural;

    // 2. Dictionary Source Priority (词库优先级与启用状态)
    struct DictConfig {
        std::string dict_id;
        bool enabled = true;
    };
    std::vector<DictConfig> dictionary_order = {
        {DICT_ID_CUSTOM, true},
        {DICT_ID_BUILTIN_WUBI, true},
        {DICT_ID_BUILTIN_PINYIN, true}
    };

    // 3. Frequency & Learning Control (词频与学习开关独立)
    bool frequency_enabled = true; // false = freeze frequency weighting, keep base/manual order
    bool learning_enabled = true;  // false = do not record commit to stats

    // 4. Candidate Page Size (Supported 5 to 9)
    uint32_t page_size = 5;

    // 5. Initial Mode Flags
    struct InitialModeConfig {
        bool chinese = true;
        bool full_width = false;
        bool chinese_punctuation = true;
    } initial_mode;

    // 6. Reverse Lookup Preview Settings
    struct ReverseLookupConfig {
        bool enabled = false;
        InputScheme target_method = InputScheme::Pinyin;
        ShuangpinLayout shuangpin_layout = ShuangpinLayout::Natural;
        std::string timing = "always"; // "always" or "composition"
    } reverse_lookup;

    // Helpers
    bool IsMethodEnabled(InputScheme scheme) const {
        for (const auto& m : method_configs) {
            if (m.scheme == scheme) return m.enabled;
        }
        return true;
    }

    void SetMethodEnabled(InputScheme scheme, bool enabled) {
        for (auto& m : method_configs) {
            if (m.scheme == scheme) {
                m.enabled = enabled;
                return;
            }
        }
        method_configs.push_back({scheme, enabled});
    }

    std::vector<InputScheme> GetEnabledInputMethods() const {
        std::vector<InputScheme> res;
        for (auto m : input_method_order) {
            if (IsMethodEnabled(m)) {
                res.push_back(m);
            }
        }
        return res;
    }

    bool IsDictEnabled(const std::string& dictId) const {
        for (const auto& d : dictionary_order) {
            if (d.dict_id == dictId) return d.enabled;
        }
        return false;
    }

    std::vector<std::string> GetEnabledDictionaryOrder() const {
        std::vector<std::string> res;
        for (const auto& d : dictionary_order) {
            if (d.enabled) res.push_back(d.dict_id);
        }
        return res;
    }

    std::vector<InputScheme> GetActiveInputMethods() const {
        std::vector<InputScheme> res;
        if (IsMethodEnabled(primary_method)) {
            res.push_back(primary_method);
        }
        if (mixed_input_enabled) {
            for (auto m : input_method_order) {
                if (IsMethodEnabled(m) && std::find(res.begin(), res.end(), m) == res.end()) {
                    res.push_back(m);
                }
            }
        }
        return res;
    }
};

} // namespace ime

