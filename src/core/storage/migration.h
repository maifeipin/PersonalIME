#pragma once

#include "sqlite_db.h"

namespace ime {

class Migration {
public:
    static bool Migrate(SqliteDb& db);
    static int GetSchemaVersion(SqliteDb& db);

private:
    static bool ApplyVersion1(SqliteDb& db);
};

} // namespace ime

