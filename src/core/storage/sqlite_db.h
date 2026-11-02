#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>
#include <stdexcept>
#include "../../third_party/sqlite/sqlite3.h"

namespace ime {

class Statement;

class SqliteDb {
public:
    SqliteDb();
    ~SqliteDb();

    SqliteDb(const SqliteDb&) = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;

    bool Open(const std::string& path);
    void Close();
    bool IsOpen() const { return m_db != nullptr; }

    bool Execute(const std::string& sql);
    int64_t LastInsertRowId() const;
    int Changes() const;

    std::unique_ptr<Statement> Prepare(const std::string& sql);

    std::recursive_mutex& GetMutex() { return m_mutex; }
    sqlite3* GetRawHandle() { return m_db; }

    class Transaction {
    public:
        explicit Transaction(SqliteDb& db);
        ~Transaction();
        bool Commit();
        void Rollback();
        bool IsStarted() const { return m_started; }
    private:
        SqliteDb& m_db;
        std::unique_lock<std::recursive_mutex> m_lock;
        int m_depth = 0;
        bool m_started = false;
        bool m_committed = false;
        bool m_rolledBack = false;
    };

private:
    sqlite3* m_db = nullptr;
    std::recursive_mutex m_mutex;
    int m_transactionDepth = 0;
};

class Statement {
public:
    Statement(sqlite3_stmt* stmt, std::recursive_mutex& mtx);
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    bool Step(); // returns true if SQLITE_ROW, false if SQLITE_DONE or error
    bool Execute(); // returns true if SQLITE_DONE (for INSERT/UPDATE/DELETE)
    void Reset();

    bool HasError() const { return m_hasError; }
    int LastErrorCode() const { return m_lastRc; }
    const std::string& LastErrorMessage() const { return m_lastErrorMsg; }

    void BindNull(int index);
    void BindInt(int index, int value);
    void BindInt64(int index, int64_t value);
    void BindDouble(int index, double value);
    void BindText(int index, const std::string& value);

    int ColumnInt(int col);
    int64_t ColumnInt64(int col);
    double ColumnDouble(int col);
    std::string ColumnText(int col);
    bool ColumnIsNull(int col);

private:
    sqlite3_stmt* m_stmt = nullptr;
    std::recursive_mutex& m_mutex;
    bool m_hasError = false;
    int m_lastRc = 0;
    std::string m_lastErrorMsg;
};

} // namespace ime
