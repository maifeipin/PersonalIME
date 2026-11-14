#pragma once
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>
#include "ipc_protocol.h"
#include "../core/types.h"
#include "../core/strategy/input_policy.h"
#include "../third_party/nlohmann/json.hpp"

namespace ime {

struct Stage7SettingsPayload {
    uint32_t revision = 0;
    std::string theme_mode = "follow_system";
    std::string preedit_display_mode = "always";
    bool unicode_injection_enabled = false;
    std::vector<std::wstring> unicode_allowed_hosts;
};

inline std::wstring NormalizeProcessNameLower(const std::wstring& name) {
    std::wstring out = name;
    for (wchar_t& ch : out) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return out;
}

inline void EncodeStage7SettingsToIpcResponse(
    const InputPolicy& policy,
    IpcKeyResponse& resp,
    size_t startHostIndex = 0)
{
    const auto& hosts = policy.unicode_injection.allowed_hosts;
    size_t idx = (startHostIndex < hosts.size()) ? startHostIndex : hosts.size();

    nlohmann::json j = {
        {"theme_mode", policy.theme_mode},
        {"preedit_display_mode", policy.preedit_display_mode},
        {"unicode_injection", {
            {"enabled", policy.unicode_injection.enabled},
            {"allowed_hosts", nlohmann::json::array()}
        }}
    };

    std::wstring payload = Utf8ToWide(j.dump());
    nlohmann::json includedHosts = nlohmann::json::array();
    while (idx < hosts.size()) {
        includedHosts.push_back(hosts[idx]);
        j["unicode_injection"]["allowed_hosts"] = includedHosts;
        std::wstring candidate = Utf8ToWide(j.dump());
        if (candidate.size() >= 1024) {
            includedHosts.erase(includedHosts.end() - 1);
            j["unicode_injection"]["allowed_hosts"] = includedHosts;
            break;
        }
        payload = std::move(candidate);
        ++idx;
    }

    resp.handled = 1;
    resp.has_commit = 0;
    resp.candidate_version = policy.revision;
    resp.total_candidates = static_cast<uint32_t>(hosts.size());
    resp.page_index = static_cast<uint32_t>(idx);
    if (idx < hosts.size()) {
        resp.status_flags |= IPC_STATUS_TRUNCATED;
    }
    size_t capLen = (payload.size() < 1023) ? payload.size() : 1023;
    resp.commit_length = static_cast<uint32_t>(capLen);
    wcsncpy_s(resp.commit_text, payload.c_str(), 1023);
    resp.commit_text[1023] = L'\0';
}

inline bool DecodeStage7SettingsFromIpcResponse(
    const IpcKeyResponse& resp,
    Stage7SettingsPayload& outSettings)
{
    if (resp.handled != 1 || resp.has_commit != 0 || resp.commit_length == 0) {
        return false;
    }
    size_t len = (resp.commit_length < 1023) ? static_cast<size_t>(resp.commit_length) : 1023;
    std::wstring wjson(resp.commit_text, len);
    std::string utf8 = WideToUtf8(wjson);
    try {
        auto j = nlohmann::json::parse(utf8);
        if (!j.is_object()) {
            return false;
        }
        Stage7SettingsPayload parsed;
        parsed.revision = resp.candidate_version;
        if (j.contains("theme_mode") && j["theme_mode"].is_string()) {
            std::string tm = j["theme_mode"].get<std::string>();
            if (tm == "follow_system" || tm == "light" || tm == "dark") {
                parsed.theme_mode = std::move(tm);
            }
        }
        if (j.contains("preedit_display_mode") && j["preedit_display_mode"].is_string()) {
            std::string pm = j["preedit_display_mode"].get<std::string>();
            if (pm == "always" || pm == "compositionless_only" || pm == "disabled") {
                parsed.preedit_display_mode = std::move(pm);
            }
        }
        if (j.contains("unicode_injection") && j["unicode_injection"].is_object()) {
            const auto& ui = j["unicode_injection"];
            if (ui.contains("enabled") && ui["enabled"].is_boolean()) {
                parsed.unicode_injection_enabled = ui["enabled"].get<bool>();
            }
            if (ui.contains("allowed_hosts") && ui["allowed_hosts"].is_array()) {
                for (const auto& item : ui["allowed_hosts"]) {
                    if (item.is_string()) {
                        std::wstring whost = NormalizeProcessNameLower(Utf8ToWide(item.get<std::string>()));
                        if (!whost.empty()) {
                            parsed.unicode_allowed_hosts.push_back(std::move(whost));
                        }
                    }
                }
            }
        }
        outSettings = std::move(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace ime
