#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "sqlite_db.h"

namespace ime {

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
    explicit PluginRepository(SqliteDb& db);

    bool GetAllPlugins(std::vector<PluginRecord>& outPlugins, bool onlyEnabled = false);
    bool GetPluginById(int64_t id, PluginRecord& outPlugin);
    bool AddPlugin(const PluginRecord& plugin, int64_t& outId);
    bool UpdatePlugin(const PluginRecord& plugin);
    bool DeletePlugin(int64_t id);

    bool AddPluginLog(int64_t pluginId, const std::string& pluginName, const std::string& commandLine,
                      const std::string& status, const std::string& message);
    bool GetPluginLogs(std::vector<PluginLogRecord>& outLogs, int limit = 100);
    bool ClearPluginLogs();

    bool ExecutePlugin(int64_t pluginId, std::string& outMessage);
    bool ExecuteCustom(const std::string& name, const std::string& execPath, const std::string& args, std::string& outMessage);

private:
    SqliteDb& m_db;
};

} // namespace ime

