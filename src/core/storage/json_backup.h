#pragma once

#include "entry_repository.h"
#include <string>

namespace ime {

class JsonBackup {
public:
    static bool ExportToFile(EntryRepository& repo, SqliteDb& db, const std::string& filePath, std::string* errorOut = nullptr);
    static bool ImportFromFile(EntryRepository& repo, SqliteDb& db, const std::string& filePath, bool replaceMode = false, std::string* errorOut = nullptr);
};

} // namespace ime

