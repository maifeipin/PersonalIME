#include "migration.h"
#include <iostream>

namespace ime {

int Migration::GetSchemaVersion(SqliteDb& db) {
    auto stmt = db.Prepare("PRAGMA user_version;");
    if (!stmt || !stmt->Step()) {
        return 0;
    }
    return stmt->ColumnInt(0);
}

bool Migration::Migrate(SqliteDb& db) {
    int currentVer = GetSchemaVersion(db);
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
    return true;
}

bool Migration::ApplyVersion1(SqliteDb& db) {
    SqliteDb::Transaction tx(db);

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
            scheme INTEGER NOT NULL DEFAULT 0,
            code TEXT NOT NULL DEFAULT '',
            entry_id TEXT NOT NULL DEFAULT '',
            committed_at INTEGER NOT NULL
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

    if (!tx.Commit()) {
        return false;
    }

    return db.Execute("PRAGMA user_version = 1;");
}

bool Migration::ApplyVersion2(SqliteDb& db) {
    SqliteDb::Transaction tx(db);

    // Inspect commit_events columns to see if scheme/code/entry_id need to be added
    bool hasScheme = false;
    {
        auto stmt = db.Prepare("PRAGMA table_info(commit_events);");
        if (stmt) {
            while (stmt->Step()) {
                std::string colName = stmt->ColumnText(1);
                if (colName == "scheme") {
                    hasScheme = true;
                    break;
                }
            }
        }
    }

    if (!hasScheme) {
        if (!db.Execute("ALTER TABLE commit_events ADD COLUMN scheme INTEGER NOT NULL DEFAULT 0;")) {
            tx.Rollback();
            return false;
        }
        if (!db.Execute("ALTER TABLE commit_events ADD COLUMN code TEXT NOT NULL DEFAULT '';")) {
            tx.Rollback();
            return false;
        }
        if (!db.Execute("ALTER TABLE commit_events ADD COLUMN entry_id TEXT NOT NULL DEFAULT '';")) {
            tx.Rollback();
            return false;
        }
    }

    if (!db.Execute("INSERT OR REPLACE INTO meta_settings (key, value) VALUES ('schema_version', '2');")) {
        tx.Rollback();
        return false;
    }

    if (!tx.Commit()) {
        return false;
    }

    return db.Execute("PRAGMA user_version = 2;");
}

} // namespace ime
