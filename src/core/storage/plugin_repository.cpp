#include "plugin_repository.h"
#include "../types.h"
#include <windows.h>
#include <shellapi.h>

namespace ime {

PluginRepository::PluginRepository(SqliteDb& db)
    : m_db(db) {
}

bool PluginRepository::GetAllPlugins(std::vector<PluginRecord>& outPlugins, bool onlyEnabled) {
    outPlugins.clear();
    std::string sql = "SELECT id, name, icon, exec_path, arguments, is_enabled, sort_order, created_at FROM plugins";
    if (onlyEnabled) {
        sql += " WHERE is_enabled = 1";
    }
    sql += " ORDER BY sort_order ASC, id ASC;";

    auto stmt = m_db.Prepare(sql);
    if (!stmt) return false;

    while (stmt->Step()) {
        PluginRecord rec;
        rec.id = stmt->ColumnInt64(0);
        rec.name = stmt->ColumnText(1);
        rec.icon = stmt->ColumnText(2);
        rec.exec_path = stmt->ColumnText(3);
        rec.arguments = stmt->ColumnText(4);
        rec.is_enabled = (stmt->ColumnInt(5) != 0);
        rec.sort_order = stmt->ColumnInt(6);
        rec.created_at = stmt->ColumnInt64(7);
        outPlugins.push_back(std::move(rec));
    }
    return !stmt->HasError();
}

bool PluginRepository::GetPluginById(int64_t id, PluginRecord& outPlugin) {
    auto stmt = m_db.Prepare("SELECT id, name, icon, exec_path, arguments, is_enabled, sort_order, created_at FROM plugins WHERE id = ?;");
    if (!stmt) return false;

    stmt->BindInt64(1, id);
    if (!stmt->Step()) return false;

    outPlugin.id = stmt->ColumnInt64(0);
    outPlugin.name = stmt->ColumnText(1);
    outPlugin.icon = stmt->ColumnText(2);
    outPlugin.exec_path = stmt->ColumnText(3);
    outPlugin.arguments = stmt->ColumnText(4);
    outPlugin.is_enabled = (stmt->ColumnInt(5) != 0);
    outPlugin.sort_order = stmt->ColumnInt(6);
    outPlugin.created_at = stmt->ColumnInt64(7);
    return true;
}

bool PluginRepository::AddPlugin(const PluginRecord& plugin, int64_t& outId) {
    auto stmt = m_db.Prepare("INSERT INTO plugins (name, icon, exec_path, arguments, is_enabled, sort_order) VALUES (?, ?, ?, ?, ?, ?);");
    if (!stmt) return false;

    stmt->BindText(1, plugin.name);
    stmt->BindText(2, plugin.icon);
    stmt->BindText(3, plugin.exec_path);
    stmt->BindText(4, plugin.arguments);
    stmt->BindInt(5, plugin.is_enabled ? 1 : 0);
    stmt->BindInt(6, plugin.sort_order);

    if (!stmt->Execute()) return false;
    outId = m_db.LastInsertRowId();
    return true;
}

bool PluginRepository::UpdatePlugin(const PluginRecord& plugin) {
    auto stmt = m_db.Prepare("UPDATE plugins SET name = ?, icon = ?, exec_path = ?, arguments = ?, is_enabled = ?, sort_order = ? WHERE id = ?;");
    if (!stmt) return false;

    stmt->BindText(1, plugin.name);
    stmt->BindText(2, plugin.icon);
    stmt->BindText(3, plugin.exec_path);
    stmt->BindText(4, plugin.arguments);
    stmt->BindInt(5, plugin.is_enabled ? 1 : 0);
    stmt->BindInt(6, plugin.sort_order);
    stmt->BindInt64(7, plugin.id);

    return stmt->Execute();
}

bool PluginRepository::DeletePlugin(int64_t id) {
    auto stmt = m_db.Prepare("DELETE FROM plugins WHERE id = ?;");
    if (!stmt) return false;

    stmt->BindInt64(1, id);
    return stmt->Execute();
}

bool PluginRepository::AddPluginLog(int64_t pluginId, const std::string& pluginName, const std::string& commandLine,
                                   const std::string& status, const std::string& message) {
    auto stmt = m_db.Prepare("INSERT INTO plugin_logs (plugin_id, plugin_name, command_line, status, message) VALUES (?, ?, ?, ?, ?);");
    if (!stmt) return false;

    stmt->BindInt64(1, pluginId);
    stmt->BindText(2, pluginName);
    stmt->BindText(3, commandLine);
    stmt->BindText(4, status);
    stmt->BindText(5, message);

    return stmt->Execute();
}

bool PluginRepository::GetPluginLogs(std::vector<PluginLogRecord>& outLogs, int limit) {
    outLogs.clear();
    if (limit <= 0) limit = 100;
    auto stmt = m_db.Prepare("SELECT id, plugin_id, plugin_name, command_line, executed_at, status, message FROM plugin_logs ORDER BY id DESC LIMIT ?;");
    if (!stmt) return false;

    stmt->BindInt(1, limit);
    while (stmt->Step()) {
        PluginLogRecord rec;
        rec.id = stmt->ColumnInt64(0);
        rec.plugin_id = stmt->ColumnInt64(1);
        rec.plugin_name = stmt->ColumnText(2);
        rec.command_line = stmt->ColumnText(3);
        rec.executed_at = stmt->ColumnInt64(4);
        rec.status = stmt->ColumnText(5);
        rec.message = stmt->ColumnText(6);
        outLogs.push_back(std::move(rec));
    }
    return !stmt->HasError();
}

bool PluginRepository::ClearPluginLogs() {
    return m_db.Execute("DELETE FROM plugin_logs;");
}

bool PluginRepository::ExecutePlugin(int64_t pluginId, std::string& outMessage) {
    PluginRecord rec;
    if (!GetPluginById(pluginId, rec)) {
        outMessage = "Plugin not found";
        return false;
    }

    return ExecuteCustom(rec.name, rec.exec_path, rec.arguments, outMessage);
}

bool PluginRepository::ExecuteCustom(const std::string& name, const std::string& execPath, const std::string& args, std::string& outMessage) {
    std::wstring wPath = Utf8ToWide(execPath);
    std::wstring wArgs = Utf8ToWide(args);
    std::string cmdLine = execPath + (args.empty() ? "" : (" " + args));

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = wPath.c_str();
    sei.lpParameters = wArgs.empty() ? nullptr : wArgs.c_str();
    sei.nShow = SW_SHOWNORMAL;

    BOOL ok = ShellExecuteExW(&sei);
    if (!ok) {
        DWORD err = GetLastError();
        outMessage = "Failed to launch process (Windows error code: " + std::to_string(err) + ")";
        AddPluginLog(0, name, cmdLine, "FAILED", outMessage);
        return false;
    }

    DWORD pid = 0;
    if (sei.hProcess) {
        pid = GetProcessId(sei.hProcess);
        CloseHandle(sei.hProcess);
    }

    outMessage = "Process launched successfully" + (pid > 0 ? " (PID: " + std::to_string(pid) + ")" : "");
    AddPluginLog(0, name, cmdLine, "SUCCESS", outMessage);
    return true;
}

} // namespace ime

