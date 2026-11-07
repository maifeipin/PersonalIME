#pragma once
#include <vector>
#include <string>
#include "ipc_protocol.h"
#include "../core/types.h"
#include "../core/storage/plugin_repository.h"
#include "../../third_party/nlohmann/json.hpp"

namespace ime {

struct DecodedPluginItem {
    uint32_t id = 0;
    std::wstring name;
    std::wstring icon;
    std::wstring exec_path;
    std::wstring arguments;
};

inline void EncodePluginsToIpcResponse(
    const std::vector<PluginRecord>& plugins,
    uint32_t revision,
    IpcKeyResponse& resp)
{
    nlohmann::json arr = nlohmann::json::array();
    size_t includedCount = 0;
    bool truncated = false;
    std::wstring lastValidWide = L"[]";

    for (const auto& p : plugins) {
        nlohmann::json item = {
            {"id", p.id},
            {"name", p.name},
            {"icon", p.icon},
            {"exec_path", p.exec_path},
            {"arguments", p.arguments}
        };
        nlohmann::json candidateArr = arr;
        candidateArr.push_back(item);
        std::string dumped = candidateArr.dump();
        std::wstring wideDumped = Utf8ToWide(dumped);
        if (wideDumped.length() >= 1024) {
            truncated = true;
            break;
        }
        arr = std::move(candidateArr);
        lastValidWide = std::move(wideDumped);
        includedCount++;
    }

    resp.has_commit = 1;
    wcsncpy_s(resp.commit_text, lastValidWide.c_str(), 1023);
    resp.commit_text[1023] = L'\0';
    resp.commit_length = static_cast<uint32_t>(wcslen(resp.commit_text));
    resp.candidate_version = revision;
    resp.total_candidates = static_cast<uint32_t>(plugins.size());
    resp.candidate_count = static_cast<uint32_t>(includedCount);
    if (truncated) {
        resp.status_flags |= IPC_STATUS_TRUNCATED;
    }
    resp.handled = 1;
}

inline bool DecodePluginsFromIpcResponse(
    const IpcKeyResponse& resp,
    std::vector<DecodedPluginItem>& outPlugins,
    bool* outTruncated = nullptr,
    uint32_t* outRevision = nullptr)
{
    if (resp.handled != 1) {
        return false;
    }

    bool truncated = ((resp.status_flags & IPC_STATUS_TRUNCATED) != 0) ||
                     (resp.candidate_count < resp.total_candidates);
    if (outTruncated) *outTruncated = truncated;
    if (outRevision) *outRevision = resp.candidate_version;

    outPlugins.clear();
    std::wstring rawWide(resp.commit_text);
    if (rawWide.empty()) {
        return resp.candidate_count == 0;
    }

    std::string utf8 = WideToUtf8(rawWide);
    auto arr = nlohmann::json::parse(utf8, nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) {
        return false;
    }

    for (const auto& item : arr) {
        if (!item.is_object()) return false;
        if (!item.contains("id") || !item["id"].is_number_integer()) return false;
        if (!item.contains("name") || !item["name"].is_string()) return false;
        if (!item.contains("icon") || !item["icon"].is_string()) return false;
        if (!item.contains("exec_path") || !item["exec_path"].is_string()) return false;

        int64_t rawId = item["id"].get<int64_t>();
        if (rawId <= 0) continue;

        DecodedPluginItem p{};
        p.id = static_cast<uint32_t>(rawId);
        p.name = Utf8ToWide(item["name"].get<std::string>());
        p.icon = Utf8ToWide(item["icon"].get<std::string>());
        p.exec_path = Utf8ToWide(item["exec_path"].get<std::string>());
        if (item.contains("arguments") && item["arguments"].is_string()) {
            p.arguments = Utf8ToWide(item["arguments"].get<std::string>());
        }
        outPlugins.push_back(std::move(p));
    }

    return true;
}

} // namespace ime

