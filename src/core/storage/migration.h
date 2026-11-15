#pragma once

#include "sqlite_db.h"

namespace ime {

class Migration {
public:
    static constexpr int CURRENT_SCHEMA_VERSION = 8;

    static bool Migrate(SqliteDb& db);
    static int GetSchemaVersion(SqliteDb& db);

private:
    static bool ApplyVersion1(SqliteDb& db);
    static bool ApplyVersion2(SqliteDb& db);
    static bool ApplyVersion3(SqliteDb& db);
    static bool ApplyVersion4(SqliteDb& db);
    static bool ApplyVersion5(SqliteDb& db);
    static bool ApplyVersion6(SqliteDb& db);
    static bool ApplyVersion7(SqliteDb& db);
    static bool ApplyVersion8(SqliteDb& db);
    static bool EnsureDefaultPluginsSeeded(SqliteDb& db);
    static bool SeedDefaultPluginsInTransaction(SqliteDb& db);
    static bool EnsureDefaultEmojiMappingsSeeded(SqliteDb& db);
    static bool SeedDefaultEmojiMappingsInTransaction(SqliteDb& db);
};

} // namespace ime

