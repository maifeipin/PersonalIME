#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <atomic>
#include "sqlite_db.h"

namespace ime {

enum class PluginExecStatus : uint32_t {
    Success      = 0,
    NotFound     = 1,
    Disabled     = 2,
    LaunchFailed = 3
};

inline const char* PluginExecStatusToString(PluginExecStatus st) {
    switch (st) {
        case PluginExecStatus::Success:      return "SUCCESS";
        case PluginExecStatus::NotFound:     return "NOT_FOUND";
        case PluginExecStatus::Disabled:     return "DISABLED";
        case PluginExecStatus::LaunchFailed: return "LAUNCH_FAILED";
        default:                             return "UNKNOWN";
    }
}

struct PluginRecord {
    int64_t id = 0;
    std::string name;
    std::string icon;
    std::string exec_path;
    std::string arguments;
    bool is_enabled = true;
    int sort_order = 0;
    int64_t created_at = 0;
};

struct PluginLogRecord {
    int64_t id = 0;
    int64_t plugin_id = 0;
    std::string plugin_name;
    std::string command_line;
    int64_t executed_at = 0;
    std::string status; // "SUCCESS" or "FAILED"
    std::string message;
};

class PluginRepository {
public:
    using ProcessLauncherFunc = std::function<bool(const std::string& execPath, const std::string& args, uint32_t& outPid, uint32_t& outWin32Error)>;

    explicit PluginRepository(SqliteDb& db);

    void SetProcessLauncher(ProcessLauncherFunc launcher) { m_processLauncher = std::move(launcher); }
    uint32_t GetRevision() const { return m_revision.load(std::memory_order_acquire); }

    bool GetAllPlugins(std::vector<PluginRecord>& outPlugins, bool onlyEnabled = false);
    bool GetPluginById(int64_t id, PluginRecord& outPlugin);
    bool AddPlugin(const PluginRecord& plugin, int64_t& outId);
    bool UpdatePlugin(const PluginRecord& plugin);
    bool DeletePlugin(int64_t id);

    bool AddPluginLog(int64_t pluginId, const std::string& pluginName, const std::string& commandLine,
                      const std::string& status, const std::string& message);
    bool GetPluginLogs(std::vector<PluginLogRecord>& outLogs, int limit = 100);
    bool ClearPluginLogs();

    bool ExecutePlugin(int64_t pluginId, std::string& outMessage, PluginExecStatus* outStatus = nullptr);
    bool ExecuteCustom(const std::string& name, const std::string& execPath, const std::string& args,
                       std::string& outMessage, int64_t pluginId = 0, PluginExecStatus* outStatus = nullptr);

private:
    SqliteDb& m_db;
    ProcessLauncherFunc m_processLauncher;
    std::atomic<uint32_t> m_revision{1};
};

} // namespace ime

