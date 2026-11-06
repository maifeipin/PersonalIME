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
            status INTEGER NOT NULL DEFAULT 1
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

} // namespace ime
