#pragma once

#include "input_policy.h"
#include "../../third_party/nlohmann/json.hpp"
#include <string>
#include <vector>
#include <set>

namespace ime {

inline std::string InputSchemeToString(InputScheme s) {
    switch (s) {
        case InputScheme::Wubi86:    return "wubi86";
        case InputScheme::Pinyin:    return "pinyin";
        case InputScheme::Shuangpin: return "shuangpin";
        case InputScheme::Custom:    return "custom";
        default:                     return "unknown";
    }
}

inline bool TryParseInputScheme(const std::string& str, InputScheme& out) {
    if (str == "wubi86" || str == "wubi") { out = InputScheme::Wubi86; return true; }
    if (str == "pinyin") { out = InputScheme::Pinyin; return true; }
    if (str == "shuangpin") { out = InputScheme::Shuangpin; return true; }
    if (str == "custom") { out = InputScheme::Custom; return true; }
    return false;
}

inline bool TryParseShuangpinLayout(const std::string& str, ShuangpinLayout& out) {
    if (str == "natural" || str == "ziranma") { out = ShuangpinLayout::Natural; return true; }
    if (str == "ms" || str == "microsoft") { out = ShuangpinLayout::Microsoft; return true; }
    if (str == "xiaohe") { out = ShuangpinLayout::Xiaohe; return true; }
    return false;
}

inline InputScheme StringToInputScheme(const std::string& str, InputScheme defaultVal = InputScheme::Wubi86) {
    InputScheme res;
    if (TryParseInputScheme(str, res)) return res;
    return defaultVal;
}

inline bool ValidateInputPolicy(const InputPolicy& p, std::string* errorOut = nullptr) {
    if (p.primary_method != InputScheme::Wubi86 &&
        p.primary_method != InputScheme::Pinyin &&
        p.primary_method != InputScheme::Shuangpin) {
        if (errorOut) *errorOut = "Invalid primary_input_method: must be 'wubi86', 'pinyin', or 'shuangpin'";
        return false;
    }

    if (p.shuangpin_layout != ShuangpinLayout::Natural &&
        p.shuangpin_layout != ShuangpinLayout::Microsoft &&
        p.shuangpin_layout != ShuangpinLayout::Xiaohe) {
        if (errorOut) *errorOut = "Invalid shuangpin_layout: must be 'natural', 'ms', or 'xiaohe'";
        return false;
    }

    if (p.page_size < 5 || p.page_size > 9) {
        if (errorOut) *errorOut = "candidate_page_size must be between 5 and 9";
        return false;
    }

    if (p.dictionary_order.empty()) {
        if (errorOut) *errorOut = "dictionary_order cannot be empty";
        return false;
    }

    if (p.input_method_order.empty()) {
        if (errorOut) *errorOut = "input_method_order cannot be empty";
        return false;
    }

    if (!p.IsMethodEnabled(p.primary_method)) {
        if (errorOut) *errorOut = "Default (primary) input method must be enabled";
        return false;
    }

    if (p.GetEnabledInputMethods().empty()) {
        if (errorOut) *errorOut = "At least one input method must be enabled";
        return false;
    }

    bool hasEnabledDict = false;
    std::set<std::string> knownDicts = { DICT_ID_CUSTOM, DICT_ID_BUILTIN_WUBI, DICT_ID_BUILTIN_PINYIN };
    std::set<std::string> seenDicts;
    for (const auto& d : p.dictionary_order) {
        if (d.dict_id.empty() || knownDicts.find(d.dict_id) == knownDicts.end()) {
            if (errorOut) *errorOut = "Unknown dictionary_id in dictionary_order: " + d.dict_id;
            return false;
        }
        if (seenDicts.find(d.dict_id) != seenDicts.end()) {
            if (errorOut) *errorOut = "Duplicate dictionary_id in dictionary_order: " + d.dict_id;
            return false;
        }
        seenDicts.insert(d.dict_id);
        if (d.enabled) hasEnabledDict = true;
    }

    if (!hasEnabledDict) {
        if (errorOut) *errorOut = "At least one dictionary must be enabled in dictionary_order";
        return false;
    }

    if (p.reverse_lookup.timing != "always" && p.reverse_lookup.timing != "composition") {
        if (errorOut) *errorOut = "reverse_lookup.timing must be 'always' or 'composition'";
        return false;
    }

    return true;
}

inline nlohmann::json InputPolicyToJson(const InputPolicy& policy) {
    nlohmann::json j;
    j["revision"] = policy.revision;
    j["primary_input_method"] = InputSchemeToString(policy.primary_method);
    j["shuangpin_layout"] = ShuangpinLayoutToString(policy.shuangpin_layout);
    j["mixed_input_enabled"] = policy.mixed_input_enabled;

    nlohmann::json methodOrder = nlohmann::json::array();
    for (auto m : policy.input_method_order) {
        methodOrder.push_back(InputSchemeToString(m));
    }
    j["input_method_order"] = methodOrder;

    nlohmann::json enabledMethods = nlohmann::json::array();
    for (auto m : policy.GetEnabledInputMethods()) {
        enabledMethods.push_back(InputSchemeToString(m));
    }
    j["enabled_input_methods"] = enabledMethods;

    nlohmann::json inputMethods = nlohmann::json::array();
    for (auto m : policy.input_method_order) {
        inputMethods.push_back({
            {"scheme", InputSchemeToString(m)},
            {"enabled", policy.IsMethodEnabled(m)}
        });
    }
    j["input_methods"] = inputMethods;

    nlohmann::json dictOrder = nlohmann::json::array();
    for (const auto& d : policy.dictionary_order) {
        dictOrder.push_back({
            {"dict_id", d.dict_id},
            {"enabled", d.enabled}
        });
    }
    j["dictionary_order"] = dictOrder;

    j["frequency_enabled"] = policy.frequency_enabled;
    j["learning_enabled"] = policy.learning_enabled;
    j["candidate_page_size"] = policy.page_size;

    j["initial_mode"] = {
        {"chinese", policy.initial_mode.chinese},
        {"full_width", policy.initial_mode.full_width},
        {"chinese_punctuation", policy.initial_mode.chinese_punctuation}
    };

    j["reverse_lookup"] = {
        {"enabled", policy.reverse_lookup.enabled},
        {"target_method", InputSchemeToString(policy.reverse_lookup.target_method)},
        {"shuangpin_layout", ShuangpinLayoutToString(policy.reverse_lookup.shuangpin_layout)},
        {"timing", policy.reverse_lookup.timing}
    };

    return j;
}

inline bool InputPolicyFromJson(const nlohmann::json& j, InputPolicy& policy, std::string* errorOut = nullptr) {
    try {
        if (!j.is_object()) {
            if (errorOut) *errorOut = "Policy must be a JSON object";
            return false;
        }

        InputPolicy p = policy; // start from current/default values

        if (j.contains("revision")) {
            const auto& v = j["revision"];
            if (!v.is_number_integer() && !v.is_number_unsigned()) {
                if (errorOut) *errorOut = "Field 'revision' must be an integer";
                return false;
            }
            if (v.is_number_integer() && v.get<int64_t>() < 0) {
                if (errorOut) *errorOut = "Field 'revision' cannot be negative";
                return false;
            }
            p.revision = v.get<uint64_t>();
        }

        if (j.contains("primary_input_method")) {
            const auto& v = j["primary_input_method"];
            if (!v.is_string()) {
                if (errorOut) *errorOut = "Field 'primary_input_method' must be a string";
                return false;
            }
            std::string s = v.get<std::string>();
            if (!TryParseInputScheme(s, p.primary_method) ||
                (p.primary_method != InputScheme::Wubi86 &&
                 p.primary_method != InputScheme::Pinyin &&
                 p.primary_method != InputScheme::Shuangpin)) {
                if (errorOut) *errorOut = "Invalid primary_input_method: " + s;
                return false;
            }
        }

        if (j.contains("shuangpin_layout")) {
            const auto& v = j["shuangpin_layout"];
            if (!v.is_string()) {
                if (errorOut) *errorOut = "Field 'shuangpin_layout' must be a string";
                return false;
            }
            std::string s = v.get<std::string>();
            if (!TryParseShuangpinLayout(s, p.shuangpin_layout)) {
                if (errorOut) *errorOut = "Invalid shuangpin_layout: " + s;
                return false;
            }
        }

        if (j.contains("mixed_input_enabled")) {
            const auto& v = j["mixed_input_enabled"];
            if (!v.is_boolean()) {
                if (errorOut) *errorOut = "Field 'mixed_input_enabled' must be a boolean";
                return false;
            }
            p.mixed_input_enabled = v.get<bool>();
        }

        if (j.contains("input_method_order")) {
            const auto& v = j["input_method_order"];
            if (!v.is_array()) {
                if (errorOut) *errorOut = "Field 'input_method_order' must be an array";
                return false;
            }
            if (v.empty()) {
                if (errorOut) *errorOut = "Field 'input_method_order' cannot be empty";
                return false;
            }
            std::vector<InputScheme> order;
            std::set<InputScheme> seen;
            for (const auto& item : v) {
                if (!item.is_string()) {
                    if (errorOut) *errorOut = "Each item in 'input_method_order' must be a string";
                    return false;
                }
                std::string s = item.get<std::string>();
                InputScheme sc;
                if (!TryParseInputScheme(s, sc) ||
                    (sc != InputScheme::Wubi86 && sc != InputScheme::Pinyin && sc != InputScheme::Shuangpin)) {
                    if (errorOut) *errorOut = "Invalid input scheme in input_method_order: " + s;
                    return false;
                }
                if (seen.find(sc) != seen.end()) {
                    if (errorOut) *errorOut = "Duplicate input scheme in input_method_order: " + s;
                    return false;
                }
                seen.insert(sc);
                order.push_back(sc);
            }
            p.input_method_order = order;
        }

        bool hasEnabledList = j.contains("enabled_input_methods");
        bool hasInputMethods = j.contains("input_methods");

        std::set<InputScheme> listEnabledSet;
        if (hasEnabledList) {
            const auto& v = j["enabled_input_methods"];
            if (!v.is_array()) {
                if (errorOut) *errorOut = "Field 'enabled_input_methods' must be an array";
                return false;
            }
            std::set<InputScheme> seen;
            for (const auto& item : v) {
                if (!item.is_string()) {
                    if (errorOut) *errorOut = "Each item in 'enabled_input_methods' must be a string";
                    return false;
                }
                std::string s = item.get<std::string>();
                InputScheme sc;
                if (!TryParseInputScheme(s, sc) ||
                    (sc != InputScheme::Wubi86 && sc != InputScheme::Pinyin && sc != InputScheme::Shuangpin)) {
                    if (errorOut) *errorOut = "Invalid scheme in enabled_input_methods: " + s;
                    return false;
                }
                if (seen.find(sc) != seen.end()) {
                    if (errorOut) *errorOut = "Duplicate scheme in enabled_input_methods: " + s;
                    return false;
                }
                seen.insert(sc);
            }
            listEnabledSet = seen;
        }

        std::map<InputScheme, bool> methodObjMap;
        if (hasInputMethods) {
            const auto& v = j["input_methods"];
            if (!v.is_array()) {
                if (errorOut) *errorOut = "Field 'input_methods' must be an array";
                return false;
            }
            std::set<InputScheme> seen;
            for (const auto& item : v) {
                if (!item.is_object()) {
                    if (errorOut) *errorOut = "Each item in 'input_methods' must be an object";
                    return false;
                }
                if (!item.contains("scheme") || !item["scheme"].is_string()) {
                    if (errorOut) *errorOut = "Each item in 'input_methods' must have string 'scheme'";
                    return false;
                }
                std::string s = item["scheme"].get<std::string>();
                InputScheme sc;
                if (!TryParseInputScheme(s, sc) ||
                    (sc != InputScheme::Wubi86 && sc != InputScheme::Pinyin && sc != InputScheme::Shuangpin)) {
                    if (errorOut) *errorOut = "Invalid scheme in input_methods: " + s;
                    return false;
                }
                if (seen.find(sc) != seen.end()) {
                    if (errorOut) *errorOut = "Duplicate scheme in input_methods: " + s;
                    return false;
                }
                seen.insert(sc);
                bool enabled = true;
                if (item.contains("enabled")) {
                    if (!item["enabled"].is_boolean()) {
                        if (errorOut) *errorOut = "Field 'enabled' in input_methods item must be a boolean";
                        return false;
                    }
                    enabled = item["enabled"].get<bool>();
                }
                methodObjMap[sc] = enabled;
            }
        }

        // Conflict detection: if BOTH are supplied, they must NOT contradict each other!
        if (hasEnabledList && hasInputMethods) {
            for (auto sc : {InputScheme::Wubi86, InputScheme::Pinyin, InputScheme::Shuangpin}) {
                bool fromList = (listEnabledSet.find(sc) != listEnabledSet.end());
                auto it = methodObjMap.find(sc);
                bool fromObj = (it != methodObjMap.end()) ? it->second : true;
                if (fromList != fromObj) {
                    if (errorOut) *errorOut = "Conflicting enabled states between 'enabled_input_methods' and 'input_methods' for scheme: " + InputSchemeToString(sc);
                    return false;
                }
            }
        }

        if (hasInputMethods) {
            for (const auto& [sc, enabled] : methodObjMap) {
                p.SetMethodEnabled(sc, enabled);
            }
        } else if (hasEnabledList) {
            for (auto sc : {InputScheme::Wubi86, InputScheme::Pinyin, InputScheme::Shuangpin}) {
                p.SetMethodEnabled(sc, listEnabledSet.find(sc) != listEnabledSet.end());
            }
        }

        if (j.contains("dictionary_order")) {
            const auto& v = j["dictionary_order"];
            if (!v.is_array()) {
                if (errorOut) *errorOut = "Field 'dictionary_order' must be an array";
                return false;
            }
            if (v.empty()) {
                if (errorOut) *errorOut = "Field 'dictionary_order' cannot be empty";
                return false;
            }
            std::vector<InputPolicy::DictConfig> dOrder;
            std::set<std::string> seenDicts;
            std::set<std::string> knownDicts = { DICT_ID_CUSTOM, DICT_ID_BUILTIN_WUBI, DICT_ID_BUILTIN_PINYIN };
            bool hasEnabled = false;

            for (const auto& item : v) {
                if (!item.is_object()) {
                    if (errorOut) *errorOut = "Each item in 'dictionary_order' must be an object";
                    return false;
                }
                if (!item.contains("dict_id") || !item["dict_id"].is_string()) {
                    if (errorOut) *errorOut = "Each item in 'dictionary_order' must have string 'dict_id'";
                    return false;
                }
                std::string did = item["dict_id"].get<std::string>();
                if (knownDicts.find(did) == knownDicts.end()) {
                    if (errorOut) *errorOut = "Unknown dict_id in dictionary_order: " + did;
                    return false;
                }
                if (seenDicts.find(did) != seenDicts.end()) {
                    if (errorOut) *errorOut = "Duplicate dict_id in dictionary_order: " + did;
                    return false;
                }
                seenDicts.insert(did);

                bool enabled = true;
                if (item.contains("enabled")) {
                    if (!item["enabled"].is_boolean()) {
                        if (errorOut) *errorOut = "Field 'enabled' in dictionary_order item must be a boolean";
                        return false;
                    }
                    enabled = item["enabled"].get<bool>();
                }
                if (enabled) hasEnabled = true;
                dOrder.push_back({ did, enabled });
            }

            if (!hasEnabled) {
                if (errorOut) *errorOut = "At least one dictionary must be enabled in dictionary_order";
                return false;
            }
            p.dictionary_order = dOrder;
        }

        if (j.contains("frequency_enabled")) {
            const auto& v = j["frequency_enabled"];
            if (!v.is_boolean()) {
                if (errorOut) *errorOut = "Field 'frequency_enabled' must be a boolean";
                return false;
            }
            p.frequency_enabled = v.get<bool>();
        }

        if (j.contains("learning_enabled")) {
            const auto& v = j["learning_enabled"];
            if (!v.is_boolean()) {
                if (errorOut) *errorOut = "Field 'learning_enabled' must be a boolean";
                return false;
            }
            p.learning_enabled = v.get<bool>();
        }

        if (j.contains("candidate_page_size")) {
            const auto& v = j["candidate_page_size"];
            if (!v.is_number_integer() && !v.is_number_unsigned()) {
                if (errorOut) *errorOut = "Field 'candidate_page_size' must be an integer";
                return false;
            }
            if (v.is_number_integer()) {
                int64_t val = v.get<int64_t>();
                if (val < 5 || val > 9) {
                    if (errorOut) *errorOut = "candidate_page_size must be between 5 and 9";
                    return false;
                }
                p.page_size = static_cast<uint32_t>(val);
            } else {
                uint64_t val = v.get<uint64_t>();
                if (val < 5 || val > 9) {
                    if (errorOut) *errorOut = "candidate_page_size must be between 5 and 9";
                    return false;
                }
                p.page_size = static_cast<uint32_t>(val);
            }
        }

        if (j.contains("initial_mode")) {
            const auto& im = j["initial_mode"];
            if (!im.is_object()) {
                if (errorOut) *errorOut = "Field 'initial_mode' must be an object";
                return false;
            }
            if (im.contains("chinese")) {
                if (!im["chinese"].is_boolean()) {
                    if (errorOut) *errorOut = "Field 'initial_mode.chinese' must be a boolean";
                    return false;
                }
                p.initial_mode.chinese = im["chinese"].get<bool>();
            }
            if (im.contains("full_width")) {
                if (!im["full_width"].is_boolean()) {
                    if (errorOut) *errorOut = "Field 'initial_mode.full_width' must be a boolean";
                    return false;
                }
                p.initial_mode.full_width = im["full_width"].get<bool>();
            }
            if (im.contains("chinese_punctuation")) {
                if (!im["chinese_punctuation"].is_boolean()) {
                    if (errorOut) *errorOut = "Field 'initial_mode.chinese_punctuation' must be a boolean";
                    return false;
                }
                p.initial_mode.chinese_punctuation = im["chinese_punctuation"].get<bool>();
            }
        }

        if (j.contains("reverse_lookup")) {
            const auto& rl = j["reverse_lookup"];
            if (!rl.is_object()) {
                if (errorOut) *errorOut = "Field 'reverse_lookup' must be an object";
                return false;
            }
            if (rl.contains("enabled")) {
                if (!rl["enabled"].is_boolean()) {
                    if (errorOut) *errorOut = "Field 'reverse_lookup.enabled' must be a boolean";
                    return false;
                }
                p.reverse_lookup.enabled = rl["enabled"].get<bool>();
            }
            if (rl.contains("target_method")) {
                if (!rl["target_method"].is_string()) {
                    if (errorOut) *errorOut = "Field 'reverse_lookup.target_method' must be a string";
                    return false;
                }
                std::string tm = rl["target_method"].get<std::string>();
                if (!TryParseInputScheme(tm, p.reverse_lookup.target_method) ||
                    (p.reverse_lookup.target_method != InputScheme::Wubi86 &&
                     p.reverse_lookup.target_method != InputScheme::Pinyin &&
                     p.reverse_lookup.target_method != InputScheme::Shuangpin)) {
                    if (errorOut) *errorOut = "Invalid reverse_lookup.target_method: " + tm;
                    return false;
                }
            }
            if (rl.contains("shuangpin_layout")) {
                if (!rl["shuangpin_layout"].is_string()) {
                    if (errorOut) *errorOut = "Field 'reverse_lookup.shuangpin_layout' must be a string";
                    return false;
                }
                std::string sl = rl["shuangpin_layout"].get<std::string>();
                if (!TryParseShuangpinLayout(sl, p.reverse_lookup.shuangpin_layout)) {
                    if (errorOut) *errorOut = "Invalid reverse_lookup.shuangpin_layout: " + sl;
                    return false;
                }
            }
            if (rl.contains("timing")) {
                if (!rl["timing"].is_string()) {
                    if (errorOut) *errorOut = "Field 'reverse_lookup.timing' must be a string";
                    return false;
                }
                std::string tm = rl["timing"].get<std::string>();
                if (tm != "always" && tm != "composition") {
                    if (errorOut) *errorOut = "reverse_lookup.timing must be 'always' or 'composition'";
                    return false;
                }
                p.reverse_lookup.timing = tm;
            }
        }

        if (!ValidateInputPolicy(p, errorOut)) {
            return false;
        }

        policy = p;
        return true;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = std::string("JSON parsing exception: ") + e.what();
        return false;
    }
}

} // namespace ime

