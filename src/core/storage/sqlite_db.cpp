#include "sqlite_db.h"
#include <iostream>

namespace ime {

SqliteDb::SqliteDb() {}

SqliteDb::~SqliteDb() {
    Close();
}

bool SqliteDb::Open(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    Close();

    int rc = sqlite3_open_v2(
        path.c_str(),
        &m_db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr
    );

    if (rc != SQLITE_OK) {
        if (m_db) {
            sqlite3_close_v2(m_db);
            m_db = nullptr;
        }
        return false;
    }

    // Recommended performance and concurrency pragmas
    char* err = nullptr;
    sqlite3_exec(m_db, "PRAGMA journal_mode = WAL;", nullptr, nullptr, &err);
    if (err) sqlite3_free(err);

    sqlite3_exec(m_db, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, &err);
    if (err) sqlite3_free(err);

    sqlite3_exec(m_db, "PRAGMA foreign_keys = ON;", nullptr, nullptr, &err);
    if (err) sqlite3_free(err);

    sqlite3_busy_timeout(m_db, 5000); // 5 seconds busy timeout
    return true;
}

void SqliteDb::Close() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_db) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }
}

bool SqliteDb::Execute(const std::string& sql) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;
    char* err = nullptr;
    int rc = sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        if (err) {
            std::cerr << "[SQLite Error: " << rc << "] " << err << " (SQL: " << sql.substr(0, 100) << "...)\n";
            sqlite3_free(err);
        }
        return false;
    }
    return true;
}

int64_t SqliteDb::LastInsertRowId() const {
    if (!m_db) return 0;
    return sqlite3_last_insert_rowid(m_db);
}

int SqliteDb::Changes() const {
    if (!m_db) return 0;
    return sqlite3_changes(m_db);
}

std::unique_ptr<Statement> SqliteDb::Prepare(const std::string& sql) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return nullptr;
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(m_db, sql.c_str(), static_cast<int>(sql.length()), &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt) {
        return nullptr;
    }
    return std::make_unique<Statement>(stmt, m_mutex);
}

SqliteDb::Transaction::Transaction(SqliteDb& db)
    : m_db(db), m_lock(db.m_mutex) {
    if (m_db.m_transactionDepth == 0) {
        m_db.Execute("BEGIN IMMEDIATE TRANSACTION;");
    } else {
        m_db.Execute("SAVEPOINT sp_" + std::to_string(m_db.m_transactionDepth) + ";");
    }
    m_depth = ++m_db.m_transactionDepth;
}

SqliteDb::Transaction::~Transaction() {
    if (!m_committed && !m_rolledBack) {
        Rollback();
    }
}

bool SqliteDb::Transaction::Commit() {
    if (m_committed || m_rolledBack) return false;
    bool ok = true;
    if (m_db.m_transactionDepth == 1) {
        ok = m_db.Execute("COMMIT TRANSACTION;");
    } else if (m_db.m_transactionDepth > 1) {
        ok = m_db.Execute("RELEASE SAVEPOINT sp_" + std::to_string(m_db.m_transactionDepth - 1) + ";");
    }
    if (m_db.m_transactionDepth > 0) {
        m_db.m_transactionDepth--;
    }
    m_committed = true;
    return ok;
}

void SqliteDb::Transaction::Rollback() {
    if (m_committed || m_rolledBack) return;
    if (m_db.m_transactionDepth == 1) {
        m_db.Execute("ROLLBACK TRANSACTION;");
    } else if (m_db.m_transactionDepth > 1) {
        m_db.Execute("ROLLBACK TO SAVEPOINT sp_" + std::to_string(m_db.m_transactionDepth - 1) + ";");
        m_db.Execute("RELEASE SAVEPOINT sp_" + std::to_string(m_db.m_transactionDepth - 1) + ";");
    }
    if (m_db.m_transactionDepth > 0) {
        m_db.m_transactionDepth--;
    }
    m_rolledBack = true;
}

// Statement Implementation
Statement::Statement(sqlite3_stmt* stmt, std::recursive_mutex& mtx)
    : m_stmt(stmt), m_mutex(mtx) {}

Statement::~Statement() {
    if (m_stmt) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        sqlite3_finalize(m_stmt);
        m_stmt = nullptr;
    }
}

bool Statement::Step() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_stmt) return false;
    int rc = sqlite3_step(m_stmt);
    return (rc == SQLITE_ROW);
}

void Statement::Reset() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_stmt) {
        sqlite3_reset(m_stmt);
        sqlite3_clear_bindings(m_stmt);
    }
}

void Statement::BindNull(int index) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_stmt) sqlite3_bind_null(m_stmt, index);
}

void Statement::BindInt(int index, int value) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_stmt) sqlite3_bind_int(m_stmt, index, value);
}

void Statement::BindInt64(int index, int64_t value) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_stmt) sqlite3_bind_int64(m_stmt, index, value);
}

void Statement::BindDouble(int index, double value) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_stmt) sqlite3_bind_double(m_stmt, index, value);
}

void Statement::BindText(int index, const std::string& value) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_stmt) sqlite3_bind_text(m_stmt, index, value.c_str(), static_cast<int>(value.length()), SQLITE_TRANSIENT);
}

int Statement::ColumnInt(int col) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_stmt) return 0;
    return sqlite3_column_int(m_stmt, col);
}

int64_t Statement::ColumnInt64(int col) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_stmt) return 0;
    return sqlite3_column_int64(m_stmt, col);
}

double Statement::ColumnDouble(int col) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_stmt) return 0.0;
    return sqlite3_column_double(m_stmt, col);
}

std::string Statement::ColumnText(int col) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_stmt) return "";
    const unsigned char* txt = sqlite3_column_text(m_stmt, col);
    if (!txt) return "";
    return reinterpret_cast<const char*>(txt);
}

bool Statement::ColumnIsNull(int col) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_stmt) return true;
    return sqlite3_column_type(m_stmt, col) == SQLITE_NULL;
}

} // namespace ime
