#include "migration.h"
#include <iostream>
#include <set>

namespace ime {

int Migration::GetSchemaVersion(SqliteDb& db) {
    auto stmt = db.Prepare("PRAGMA user_version;");
    if (!stmt || !stmt->Step()) {
        return -1;
    }
    return stmt->ColumnInt(0);
}

bool Migration::Migrate(SqliteDb& db) {
    int currentVer = GetSchemaVersion(db);
    if (currentVer < 0 || currentVer > CURRENT_SCHEMA_VERSION) return false;
    if (currentVer < 1) {
        if (!ApplyVersion1(db)) {
            return false;
        }
    }

    bool needsV2 = (currentVer < 2);
    if (!needsV2) {
        // Verify commit_events table structure in case of v1 table recreation
        auto stmt = db.Prepare("PRAGMA table_info(commit_events);");
        if (stmt) {
            bool hasScheme = false;
            while (stmt->Step()) {
                if (stmt->ColumnText(1) == "scheme") {
                    hasScheme = true;
                    break;
                }
            }
            if (!hasScheme) {
                needsV2 = true;
            }
        }
    }

    if (needsV2) {
        if (!ApplyVersion2(db)) {
            return false;
        }
    }

    bool needsV3 = (currentVer < 3);
    if (!needsV3) {
        // Verify pending_commits table and all required columns exist
        auto stmtPending = db.Prepare("PRAGMA table_info(pending_commits);");
        if (!stmtPending) {
            return false;
        }
        std::set<std::string> pendingCols;
        while (stmtPending->Step()) {
            pendingCols.insert(stmtPending->ColumnText(1));
        }
        if (stmtPending->HasError() || pendingCols.empty()) {
            return false;
        }
        static const char* reqPendingCols[] = {
            "event_id", "session_id", "context_id", "scheme",
            "query_code", "entry_id", "text", "created_at"
        };
        for (const char* col : reqPendingCols) {
            if (pendingCols.find(col) == pendingCols.end()) {
                // Essential structure broken: cannot safely proceed with unverified pending log
                return false;
            }
        }

        // Verify commit_events table structure
        auto stmtCe = db.Prepare("PRAGMA table_info(commit_events);");
        if (!stmtCe) {
            return false;
        }
        std::set<std::string> ceCols;
        while (stmtCe->Step()) {
            ceCols.insert(stmtCe->ColumnText(1));
        }
        if (stmtCe->HasError()) {
            return false;
        }
        static const char* reqCeCols[] = {
            "session_id", "context_id", "status"
        };
        for (const char* col : reqCeCols) {
            if (ceCols.find(col) == ceCols.end()) {
                needsV3 = true;
                break;
            }
        }
    }

    if (needsV3) {
        if (!ApplyVersion3(db)) {
            return false;
        }
    }

    bool needsV4 = (currentVer < 4);
    if (!needsV4) {
        // Verify pending_commits columns and entry_resets table exist
        auto stmtPending = db.Prepare("PRAGMA table_info(pending_commits);");
        if (!stmtPending) {
            return false;
        }
        std::set<std::string> pendingCols;
        while (stmtPending->Step()) {
            pendingCols.insert(stmtPending->ColumnText(1));
        }
        if (stmtPending->HasError() || pendingCols.empty()) {
            return false;
        }
        static const char* reqPendingCols[] = {
            "learning_enabled", "layout_id", "matched_code", "resolved_code", "global_epoch", "entry_epoch"
        };
        for (const char* col : reqPendingCols) {
            if (pendingCols.find(col) == pendingCols.end()) {
                needsV4 = true;
                break;
            }
        }
    }

    if (needsV4) {
        if (!ApplyVersion4(db)) {
            return false;
        }
    }

    bool needsV5 = (currentVer < 5);
    if (!needsV5) {
        auto stmt = db.Prepare("PRAGMA table_info(plugins);");
        if (!stmt || !stmt->Step()) {
            needsV5 = true;
        }
    }

    if (needsV5) {
        if (!ApplyVersion5(db)) {
            return false;
        }
    }

    bool needsV6 = (currentVer < 6);
    if (!needsV6) {
        auto stmt = db.Prepare("PRAGMA table_info(commit_events);");
        if (!stmt) {
            return false;
        }
        bool hasPayloadHash = false;
        while (stmt->Step()) {
            if (stmt->ColumnText(1) == "payload_hash") {
                hasPayloadHash = true;
                break;
            }
        }
        if (stmt->HasError() || !hasPayloadHash) {
            needsV6 = true;
        }
    }

    if (needsV6) {
        if (!ApplyVersion6(db)) {
            return false;
        }
    }

    bool needsV7 = (currentVer < 7);
    if (!needsV7) {
        auto checkCol = [&](const char* table, const char* colName) -> bool {
            std::string sql = std::string("PRAGMA table_info(") + table + ");";
            auto stmt = db.Prepare(sql);
            if (!stmt) return false;
            while (stmt->Step()) {
                if (stmt->ColumnText(1) == colName) return true;
            }
            return false;
        };
        if (!checkCol("commit_events", "final_disposition") ||
            !checkCol("commit_events", "context_epoch") ||
            !checkCol("commit_events", "input_generation") ||
            !checkCol("commit_events", "owner_instance_id") ||
            !checkCol("pending_commits", "context_epoch") ||
            !checkCol("pending_commits", "input_generation") ||
            !checkCol("pending_commits", "owner_instance_id")) {
            needsV7 = true;
        }
    }

    if (needsV7) {
        if (!ApplyVersion7(db)) {
            return false;
        }
    }

    bool needsV8 = (currentVer < 8);
    if (!needsV8) {
        auto stmt = db.Prepare("SELECT name FROM sqlite_master WHERE type='table' AND name='emoji_mappings';");
        if (!stmt || !stmt->Step()) {
            needsV8 = true;
        }
    }
    if (needsV8) {
        if (!ApplyVersion8(db)) {
            return false;
        }
    }

    if (!EnsureDefaultPluginsSeeded(db)) {
        return false;
    }
    if (!EnsureDefaultEmojiMappingsSeeded(db)) {
        return false;
    }

    return true;
}

bool Migration::ApplyVersion1(SqliteDb& db) {
    SqliteDb::Transaction tx(db);
    if (!tx.IsStarted()) return false;

    const char* schemaV1 = R"SQL(
        CREATE TABLE IF NOT EXISTS entries (
            id TEXT PRIMARY KEY,
            text TEXT NOT NULL,
            source INTEGER NOT NULL DEFAULT 1,
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS entry_codes (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            entry_id TEXT NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
            scheme INTEGER NOT NULL,
            code TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            UNIQUE(entry_id, scheme, code)
        );
        CREATE INDEX IF NOT EXISTS idx_entry_codes_lookup ON entry_codes(scheme, code);

        CREATE TABLE IF NOT EXISTS sort_preferences (
            scheme INTEGER NOT NULL,
            code TEXT NOT NULL,
            entry_id TEXT NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
            is_pinned INTEGER NOT NULL DEFAULT 0,
            order_rank INTEGER NOT NULL DEFAULT 0,
            updated_at INTEGER NOT NULL,
            PRIMARY KEY(scheme, code, entry_id)
        );

        CREATE TABLE IF NOT EXISTS entry_stats (
            scheme INTEGER NOT NULL,
            code TEXT NOT NULL,
            entry_id TEXT NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
            selected_count INTEGER NOT NULL DEFAULT 0,
            last_selected_at INTEGER,
            effective_count REAL NOT NULL DEFAULT 0.0,
            last_decay_at INTEGER,
            PRIMARY KEY(scheme, code, entry_id)
        );

        CREATE TABLE IF NOT EXISTS commit_events (
            event_id TEXT PRIMARY KEY,
            session_id INTEGER NOT NULL DEFAULT 0,
            context_id INTEGER NOT NULL DEFAULT 0,
            scheme INTEGER NOT NULL DEFAULT 0,
            code TEXT NOT NULL DEFAULT '',
            entry_id TEXT NOT NULL DEFAULT '',
            committed_at INTEGER NOT NULL,
            status INTEGER NOT NULL DEFAULT 1,
            payload_hash TEXT NOT NULL DEFAULT ''
        );

        CREATE TABLE IF NOT EXISTS pending_commits (
            event_id TEXT PRIMARY KEY,
            session_id INTEGER NOT NULL,
            context_id INTEGER NOT NULL,
            scheme INTEGER NOT NULL,
            query_code TEXT NOT NULL,
            entry_id TEXT NOT NULL,
            text TEXT NOT NULL,
            created_at INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS meta_settings (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );

        INSERT OR IGNORE INTO meta_settings (key, value) VALUES ('schema_version', '1');
        INSERT OR IGNORE INTO meta_settings (key, value) VALUES ('dict_version', '1.0');
    )SQL";

    if (!db.Execute(schemaV1)) {
        tx.Rollback();
        return false;
    }

    if (!db.Execute("PRAGMA user_version = 1;")) return false;
    return tx.Commit();
}

bool Migration::ApplyVersion2(SqliteDb& db) {
    SqliteDb::Transaction tx(db);
    if (!tx.IsStarted()) return false;

    // Inspect commit_events columns to see if scheme/code/entry_id need to be added
    bool hasScheme = false, hasCode = false, hasEntryId = false;
    {
        auto stmt = db.Prepare("PRAGMA table_info(commit_events);");
        if (!stmt) return false;
        {
            while (stmt->Step()) {
                std::string colName = stmt->ColumnText(1);
                if (colName == "scheme") {
                    hasScheme = true;
                }
                if (colName == "code") hasCode = true;
                if (colName == "entry_id") hasEntryId = true;
            }
            if (stmt->HasError()) return false;
        }
    }

    if (!hasScheme) {
        if (!db.Execute("ALTER TABLE commit_events ADD COLUMN scheme INTEGER NOT NULL DEFAULT 0;")) {
            tx.Rollback();
            return false;
        }
    }
    if (!hasCode) {
        if (!db.Execute("ALTER TABLE commit_events ADD COLUMN code TEXT NOT NULL DEFAULT '';")) {
            tx.Rollback();
            return false;
        }
    }
    if (!hasEntryId) {
        if (!db.Execute("ALTER TABLE commit_events ADD COLUMN entry_id TEXT NOT NULL DEFAULT '';")) {
            tx.Rollback();
            return false;
        }
    }

    const char* schemaPending = R"SQL(
        CREATE TABLE IF NOT EXISTS pending_commits (
            event_id TEXT PRIMARY KEY,
            session_id INTEGER NOT NULL,
            context_id INTEGER NOT NULL,
            scheme INTEGER NOT NULL,
            query_code TEXT NOT NULL,
            entry_id TEXT NOT NULL,
            text TEXT NOT NULL,
            created_at INTEGER NOT NULL
        );
    )SQL";
    if (!db.Execute(schemaPending)) {
        tx.Rollback();
        return false;
    }

    if (!db.Execute("INSERT OR REPLACE INTO meta_settings (key, value) VALUES ('schema_version', '2');")) {
        tx.Rollback();
        return false;
    }

    if (!db.Execute("PRAGMA user_version = 2;")) return false;
    return tx.Commit();
}

bool Migration::ApplyVersion3(SqliteDb& db) {
    SqliteDb::Transaction tx(db);
    if (!tx.IsStarted()) return false;

    // 1. Ensure pending_commits exists
    const char* schemaPending = R"SQL(
        CREATE TABLE IF NOT EXISTS pending_commits (
            event_id TEXT PRIMARY KEY,
            session_id INTEGER NOT NULL,
            context_id INTEGER NOT NULL,
            scheme INTEGER NOT NULL,
            query_code TEXT NOT NULL,
            entry_id TEXT NOT NULL,
            text TEXT NOT NULL,
            created_at INTEGER NOT NULL
        );
    )SQL";
    if (!db.Execute(schemaPending)) {
        tx.Rollback();
        return false;
    }

    // 2. Add session_id, context_id, status to commit_events if not present
    bool hasSessionId = false, hasContextId = false, hasStatus = false;
    {
        auto stmt = db.Prepare("PRAGMA table_info(commit_events);");
        if (!stmt) {
            tx.Rollback();
            return false;
        }
        while (stmt->Step()) {
            std::string colName = stmt->ColumnText(1);
            if (colName == "session_id") hasSessionId = true;
            if (colName == "context_id") hasContextId = true;
            if (colName == "status") hasStatus = true;
        }
        if (stmt->HasError()) {
            tx.Rollback();
            return false;
        }
    }

    if (!hasSessionId) {
        if (!db.Execute("ALTER TABLE commit_events ADD COLUMN session_id INTEGER NOT NULL DEFAULT 0;")) {
            tx.Rollback();
            return false;
        }
    }
    if (!hasContextId) {
        if (!db.Execute("ALTER TABLE commit_events ADD COLUMN context_id INTEGER NOT NULL DEFAULT 0;")) {
            tx.Rollback();
            return false;
        }
    }
    if (!hasStatus) {
        if (!db.Execute("ALTER TABLE commit_events ADD COLUMN status INTEGER NOT NULL DEFAULT 1;")) {
            tx.Rollback();
            return false;
        }
    }

    if (!db.Execute("INSERT OR REPLACE INTO meta_settings (key, value) VALUES ('schema_version', '3');")) {
        tx.Rollback();
        return false;
    }

    if (!db.Execute("PRAGMA user_version = 3;")) {
        tx.Rollback();
        return false;
    }
    return tx.Commit();
}

bool Migration::ApplyVersion4(SqliteDb& db) {
    SqliteDb::Transaction tx(db);
    if (!tx.IsStarted()) return false;

    // 1. Ensure entry_resets table
    const char* schemaEntryResets = R"SQL(
        CREATE TABLE IF NOT EXISTS entry_resets (
            entry_id TEXT PRIMARY KEY,
            reset_epoch INTEGER NOT NULL DEFAULT 1
        );
    )SQL";
    if (!db.Execute(schemaEntryResets)) {
        tx.Rollback();
        return false;
    }

    // 2. Ensure meta_settings has global_stats_reset_epoch
    if (!db.Execute("INSERT OR IGNORE INTO meta_settings (key, value) VALUES ('global_stats_reset_epoch', '1');")) {
        tx.Rollback();
        return false;
    }

    // 3. Add columns to pending_commits if missing
    std::set<std::string> pendingCols;
    {
        auto stmt = db.Prepare("PRAGMA table_info(pending_commits);");
        if (!stmt) {
            tx.Rollback();
            return false;
        }
        while (stmt->Step()) {
            pendingCols.insert(stmt->ColumnText(1));
        }
        if (stmt->HasError()) {
            tx.Rollback();
            return false;
        }
    }

    if (pendingCols.find("learning_enabled") == pendingCols.end()) {
        if (!db.Execute("ALTER TABLE pending_commits ADD COLUMN learning_enabled INTEGER NOT NULL DEFAULT 1;")) {
            tx.Rollback();
            return false;
        }
    }
    if (pendingCols.find("layout_id") == pendingCols.end()) {
        if (!db.Execute("ALTER TABLE pending_commits ADD COLUMN layout_id TEXT NOT NULL DEFAULT '';")) {
            tx.Rollback();
            return false;
        }
    }
    if (pendingCols.find("matched_code") == pendingCols.end()) {
        if (!db.Execute("ALTER TABLE pending_commits ADD COLUMN matched_code TEXT NOT NULL DEFAULT '';")) {
            tx.Rollback();
            return false;
        }
    }
    if (pendingCols.find("resolved_code") == pendingCols.end()) {
        if (!db.Execute("ALTER TABLE pending_commits ADD COLUMN resolved_code TEXT NOT NULL DEFAULT '';")) {
            tx.Rollback();
            return false;
        }
    }
    if (pendingCols.find("global_epoch") == pendingCols.end()) {
        if (!db.Execute("ALTER TABLE pending_commits ADD COLUMN global_epoch INTEGER NOT NULL DEFAULT 1;")) {
            tx.Rollback();
            return false;
        }
    }
    if (pendingCols.find("entry_epoch") == pendingCols.end()) {
        if (!db.Execute("ALTER TABLE pending_commits ADD COLUMN entry_epoch INTEGER NOT NULL DEFAULT 1;")) {
            tx.Rollback();
            return false;
        }
    }

    if (!db.Execute("INSERT OR REPLACE INTO meta_settings (key, value) VALUES ('schema_version', '4');")) {
        tx.Rollback();
        return false;
    }

    if (!db.Execute("PRAGMA user_version = 4;")) {
        tx.Rollback();
        return false;
    }
    return tx.Commit();
}

bool Migration::SeedDefaultPluginsInTransaction(SqliteDb& db) {
    auto seededStmt = db.Prepare("SELECT value FROM meta_settings WHERE key = 'default_plugins_seeded';");
    if (!seededStmt) {
        return false;
    }
    bool hasRow = seededStmt->Step();
    if (seededStmt->HasError()) {
        return false;
    }
    if (hasRow && seededStmt->ColumnText(0) == "1") {
        return true;
    }

    struct DefaultPreset {
        const char* name;
        const char* icon;
        const char* exec_path;
        const char* arguments;
        int sort_order;
    };
    static const DefaultPreset kPresets[] = {
        {"计算器", "🧮", "calc.exe", "", 0},
        {"画图", "🎨", "mspaint.exe", "", 1},
        {"Edge浏览器", "🌐", "msedge.exe", "https://maifeipin.com", 2}
    };

    for (const auto& preset : kPresets) {
        auto checkStmt = db.Prepare("SELECT 1 FROM plugins WHERE name = ? LIMIT 1;");
        if (!checkStmt) {
            return false;
        }
        checkStmt->BindText(1, preset.name);
        bool exists = checkStmt->Step();
        if (checkStmt->HasError()) {
            return false;
        }
        if (!exists) {
            auto insStmt = db.Prepare(
                "INSERT INTO plugins (name, icon, exec_path, arguments, is_enabled, sort_order) "
                "VALUES (?, ?, ?, ?, 1, ?);"
            );
            if (!insStmt) {
                return false;
            }
            insStmt->BindText(1, preset.name);
            insStmt->BindText(2, preset.icon);
            insStmt->BindText(3, preset.exec_path);
            insStmt->BindText(4, preset.arguments);
            insStmt->BindInt(5, preset.sort_order);
            if (!insStmt->Execute()) {
                return false;
            }
        }
    }

    if (!db.Execute("INSERT OR REPLACE INTO meta_settings (key, value) VALUES ('default_plugins_seeded', '1');")) {
        return false;
    }
    return true;
}

bool Migration::EnsureDefaultPluginsSeeded(SqliteDb& db) {
    auto seededStmt = db.Prepare("SELECT value FROM meta_settings WHERE key = 'default_plugins_seeded';");
    if (!seededStmt) {
        return false;
    }
    bool hasRow = seededStmt->Step();
    if (seededStmt->HasError()) {
        return false;
    }
    if (hasRow && seededStmt->ColumnText(0) == "1") {
        return true;
    }
    seededStmt.reset();

    SqliteDb::Transaction tx(db);
    if (!tx.IsStarted()) {
        return false;
    }
    if (!SeedDefaultPluginsInTransaction(db)) {
        tx.Rollback();
        return false;
    }
    return tx.Commit();
}

bool Migration::ApplyVersion5(SqliteDb& db) {
    SqliteDb::Transaction tx(db);
    if (!tx.IsStarted()) return false;

    const char* schemaV5 = R"SQL(
        CREATE TABLE IF NOT EXISTS plugins (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            icon TEXT NOT NULL,
            exec_path TEXT NOT NULL,
            arguments TEXT NOT NULL DEFAULT '',
            is_enabled INTEGER NOT NULL DEFAULT 1,
            sort_order INTEGER NOT NULL DEFAULT 0,
            created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
        );

        CREATE TABLE IF NOT EXISTS plugin_logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            plugin_id INTEGER,
            plugin_name TEXT NOT NULL,
            command_line TEXT NOT NULL,
            executed_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
            status TEXT NOT NULL,
            message TEXT NOT NULL DEFAULT ''
        );
    )SQL";

    if (!db.Execute(schemaV5)) {
        tx.Rollback();
        return false;
    }

    if (!SeedDefaultPluginsInTransaction(db)) {
        tx.Rollback();
        return false;
    }

    if (!db.Execute("INSERT OR REPLACE INTO meta_settings (key, value) VALUES ('schema_version', '5');")) {
        tx.Rollback();
        return false;
    }

    if (!db.Execute("PRAGMA user_version = 5;")) {
        tx.Rollback();
        return false;
    }

    return tx.Commit();
}

bool Migration::ApplyVersion6(SqliteDb& db) {
    SqliteDb::Transaction tx(db);
    if (!tx.IsStarted()) return false;

    bool hasPayloadHash = false;
    {
        auto stmt = db.Prepare("PRAGMA table_info(commit_events);");
        if (!stmt) {
            tx.Rollback();
            return false;
        }
        while (stmt->Step()) {
            if (stmt->ColumnText(1) == "payload_hash") {
                hasPayloadHash = true;
                break;
            }
        }
        if (stmt->HasError()) {
            tx.Rollback();
            return false;
        }
    }

    if (!hasPayloadHash) {
        if (!db.Execute("ALTER TABLE commit_events ADD COLUMN payload_hash TEXT NOT NULL DEFAULT '';")) {
            tx.Rollback();
            return false;
        }
    }

    if (!db.Execute("INSERT OR REPLACE INTO meta_settings (key, value) VALUES ('schema_version', '6');")) {
        tx.Rollback();
        return false;
    }

    if (!db.Execute("PRAGMA user_version = 6;")) {
        tx.Rollback();
        return false;
    }

    return tx.Commit();
}

bool Migration::ApplyVersion7(SqliteDb& db) {
    SqliteDb::Transaction tx(db);
    if (!tx.IsStarted()) return false;

    auto ensureColumn = [&](const char* table, const char* colName, const char* alterSql) -> bool {
        std::string pragmaSql = std::string("PRAGMA table_info(") + table + ");";
        auto stmt = db.Prepare(pragmaSql);
        if (!stmt) return false;
        bool exists = false;
        while (stmt->Step()) {
            if (stmt->ColumnText(1) == colName) {
                exists = true;
                break;
            }
        }
        if (stmt->HasError()) return false;
        if (!exists) {
            if (!db.Execute(alterSql)) return false;
        }
        return true;
    };

    if (!ensureColumn("commit_events", "final_disposition", "ALTER TABLE commit_events ADD COLUMN final_disposition INTEGER NOT NULL DEFAULT 0;") ||
        !ensureColumn("commit_events", "context_epoch",     "ALTER TABLE commit_events ADD COLUMN context_epoch INTEGER NOT NULL DEFAULT 0;") ||
        !ensureColumn("commit_events", "input_generation",  "ALTER TABLE commit_events ADD COLUMN input_generation INTEGER NOT NULL DEFAULT 0;") ||
        !ensureColumn("commit_events", "owner_instance_id", "ALTER TABLE commit_events ADD COLUMN owner_instance_id INTEGER NOT NULL DEFAULT 0;") ||
        !ensureColumn("pending_commits", "context_epoch",     "ALTER TABLE pending_commits ADD COLUMN context_epoch INTEGER NOT NULL DEFAULT 0;") ||
        !ensureColumn("pending_commits", "input_generation",  "ALTER TABLE pending_commits ADD COLUMN input_generation INTEGER NOT NULL DEFAULT 0;") ||
        !ensureColumn("pending_commits", "owner_instance_id", "ALTER TABLE pending_commits ADD COLUMN owner_instance_id INTEGER NOT NULL DEFAULT 0;")) {
        tx.Rollback();
        return false;
    }

    if (!db.Execute("INSERT OR REPLACE INTO meta_settings (key, value) VALUES ('schema_version', '7');")) {
        tx.Rollback();
        return false;
    }

    if (!db.Execute("PRAGMA user_version = 7;")) {
        tx.Rollback();
        return false;
    }

    return tx.Commit();
}

bool Migration::ApplyVersion8(SqliteDb& db) {
    SqliteDb::Transaction tx(db);
    if (!tx.IsStarted()) return false;

    const char* sql = R"SQL(
        CREATE TABLE IF NOT EXISTS emoji_mappings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            trigger_type TEXT NOT NULL CHECK(trigger_type IN ('code', 'text')),
            trigger_key TEXT NOT NULL,
            emoji TEXT NOT NULL,
            label TEXT NOT NULL DEFAULT '',
            priority INTEGER NOT NULL DEFAULT 100,
            enabled INTEGER NOT NULL DEFAULT 1,
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL,
            UNIQUE(trigger_type, trigger_key, emoji)
        );
        CREATE INDEX IF NOT EXISTS idx_emoji_mappings_lookup
            ON emoji_mappings(enabled, trigger_type, trigger_key, priority DESC, id ASC);
    )SQL";

    if (!db.Execute(sql)) {
        tx.Rollback();
        return false;
    }

    if (!db.Execute("INSERT OR REPLACE INTO meta_settings (key, value) VALUES ('schema_version', '8');")) {
        tx.Rollback();
        return false;
    }

    if (!db.Execute("PRAGMA user_version = 8;")) {
        tx.Rollback();
        return false;
    }

    return tx.Commit();
}

bool Migration::SeedDefaultEmojiMappingsInTransaction(SqliteDb& db) {
    auto checkStmt = db.Prepare("SELECT value FROM meta_settings WHERE key = 'default_emoji_mappings_seeded';");
    if (!checkStmt) return false;
    if (checkStmt->Step() && checkStmt->ColumnText(0) == "1") {
        return true;
    }
    if (checkStmt->HasError()) return false;

    struct DefaultEmojiSeed {
        const char* type;
        const char* key;
        const char* emoji;
        const char* label;
        int priority;
    };
    static const DefaultEmojiSeed kSeeds[] = {
        { "text", "\xe5\xbc\x80\xe5\xbf\x83", "\xf0\x9f\x98\x8a", "\xe5\xbe\xae\xe7\xac\x91", 200 }, // 开心 -> 😊
        { "text", "\xe9\xab\x98\xe5\x85\xb4", "\xf0\x9f\x98\x84", "\xe5\xa4\xa7\xe7\xac\x91", 200 }, // 高兴 -> 😄
        { "text", "\xe5\xa4\xa7\xe7\xac\x91", "\xf0\x9f\x98\x82", "\xe7\xac\x91\xe5\x93\xad", 200 }, // 大笑 -> 😂
        { "text", "\xe5\x93\x88\xe5\x93\x88", "\xf0\x9f\x98\x82", "\xe7\xac\x91\xe5\x93\xad", 200 }, // 哈哈 -> 😂
        { "text", "\xe8\xb5\x9e", "\xf0\x9f\x91\x8d", "\xe7\x82\xb9\xe8\xb5\x9e", 200 },         // 赞 -> 👍
        { "text", "\xe7\x82\xb9\xe8\xb5\x9e", "\xf0\x9f\x91\x8d", "\xe7\x82\xb9\xe8\xb5\x9e", 200 }, // 点赞 -> 👍
        { "text", "\xe5\x8a\xa0\xe6\xb2\xb9", "\xf0\x9f\x92\xaa", "\xe5\x8a\xa0\xe6\xb2\xb9", 200 }, // 加油 -> 💪
        { "text", "\xe8\xb0\xa2\xe8\xb0\xa2", "\xf0\x9f\x99\x8f", "\xe6\x84\x9f\xe8\xb0\xa2", 200 }, // 谢谢 -> 🙏
        { "text", "\xe6\x84\x9f\xe8\xb0\xa2", "\xf0\x9f\x99\x8f", "\xe6\x84\x9f\xe8\xb0\xa2", 200 }, // 感谢 -> 🙏
        { "text", "\xe7\x88\xb1\xe5\xbf\x83", "\xe2\x9d\xa4\xef\xb8\x8f", "\xe7\x88\xb1\xe5\xbf\x83", 200 }, // 爱心 -> ❤️
        { "text", "\xe5\x96\x9c\xe6\xac\xa2", "\xe2\x9d\xa4\xef\xb8\x8f", "\xe7\x88\xb1\xe5\xbf\x83", 200 }, // 喜欢 -> ❤️
        { "text", "\xe5\xa5\xbd\xe7\x9a\x84", "\xf0\x9f\x91\x8c", "OK", 200 },                       // 好的 -> 👌
        { "text", "\xe6\x94\xb6\xe5\x88\xb0", "\xf0\x9f\xab\xa1", "\xe6\x94\xb6\xe5\x88\xb0", 200 }, // 收到 -> 🫡
        { "text", "\xe5\xba\x86\xe7\xa5\x9d", "\xf0\x9f\x8e\x89", "\xe5\xba\x86\xe7\xa5\x9d", 200 }, // 庆祝 -> 🎉
        { "text", "\xe6\x81\xad\xe5\x96\x9c", "\xf0\x9f\x8e\x89", "\xe5\xba\x86\xe7\xa5\x9d", 200 }, // 恭喜 -> 🎉
        { "text", "\xe7\x81\xab\xe7\xae\xad", "\xf0\x9f\x9a\x80", "\xe7\x81\xab\xe7\xae\xad", 200 }, // 火箭 -> 🚀
        { "text", "\xe9\x9a\xbe\xe8\xbf\x87", "\xf0\x9f\x98\xa2", "\xe9\x9a\xbe\xe8\xbf\x87", 200 }, // 难过 -> 😢
        { "text", "\xe7\x96\x91\xe6\x83\x91", "\xf0\x9f\xa4\x94", "\xe6\x80\x9d\xe8\x80\x83", 200 }, // 疑惑 -> 🤔
        { "text", "\xe6\x80\x9d\xe8\x80\x83", "\xf0\x9f\xa4\x94", "\xe6\x80\x9d\xe8\x80\x83", 200 }, // 思考 -> 🤔
        { "text", "\xe9\xbc\x93\xe6\x8e\x8c", "\xf0\x9f\x91\x8f", "\xe9\xbc\x93\xe6\x8e\x8c", 200 }, // 鼓掌 -> 👏
        { "text", "\xe6\x8f\xa1\xe6\x89\x8b", "\xf0\x9f\xa4\x9d", "\xe6\x8f\xa1\xe6\x89\x8b", 200 }, // 握手 -> 🤝
        { "text", "\xe5\x92\x96\xe5\x95\xa1", "\xe2\x98\x95", "\xe5\x92\x96\xe5\x95\xa1", 200 },     // 咖啡 -> ☕
        { "text", "\xe5\x86\x8d\xe8\xa7\x81", "\xf0\x9f\x91\x8b", "\xe5\x86\x8d\xe8\xa7\x81", 200 }, // 再见 -> 👋
        { "text", "\xe6\x99\x9a\xe5\xae\x89", "\xf0\x9f\x8c\x99", "\xe6\x99\x9a\xe5\xae\x89", 200 }, // 晚安 -> 🌙
        { "code", "kaixin", "\xf0\x9f\x98\x8a", "\xe5\xbe\xae\xe7\xac\x91", 250 },                   // kaixin -> 😊
        { "code", "haha", "\xf0\x9f\x98\x82", "\xe7\xac\x91\xe5\x93\xad", 250 },                     // haha -> 😂
        { "code", "zan", "\xf0\x9f\x91\x8d", "\xe7\x82\xb9\xe8\xb5\x9e", 250 },                      // zan -> 👍
        { "code", "jiayou", "\xf0\x9f\x92\xaa", "\xe5\x8a\xa0\xe6\xb2\xb9", 250 },                   // jiayou -> 💪
        { "code", "xiexie", "\xf0\x9f\x99\x8f", "\xe6\x84\x9f\xe8\xb0\xa2", 250 },                   // xiexie -> 🙏
        { "code", "bq", "\xf0\x9f\x98\x8a", "\xe8\xa1\xa8\xe6\x83\x85", 250 }                        // bq -> 😊
    };

    const char* insSql =
        "INSERT OR IGNORE INTO emoji_mappings (trigger_type, trigger_key, emoji, label, priority, enabled, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, 1, strftime('%s','now'), strftime('%s','now'));";
    for (const auto& s : kSeeds) {
        auto stmt = db.Prepare(insSql);
        if (!stmt) return false;
        stmt->BindText(1, s.type);
        stmt->BindText(2, s.key);
        stmt->BindText(3, s.emoji);
        stmt->BindText(4, s.label);
        stmt->BindInt(5, s.priority);
        if (!stmt->Execute()) return false;
    }

    return db.Execute("INSERT OR REPLACE INTO meta_settings (key, value) VALUES ('default_emoji_mappings_seeded', '1');");
}

bool Migration::EnsureDefaultEmojiMappingsSeeded(SqliteDb& db) {
    auto seededStmt = db.Prepare("SELECT value FROM meta_settings WHERE key = 'default_emoji_mappings_seeded';");
    if (!seededStmt) {
        return false;
    }
    bool hasRow = seededStmt->Step();
    if (seededStmt->HasError()) {
        return false;
    }
    if (hasRow && seededStmt->ColumnText(0) == "1") {
        return true;
    }
    seededStmt.reset();

    SqliteDb::Transaction tx(db);
    if (!tx.IsStarted()) return false;
    if (!SeedDefaultEmojiMappingsInTransaction(db)) {
        tx.Rollback();
        return false;
    }
    return tx.Commit();
}

} // namespace ime
