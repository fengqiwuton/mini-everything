#pragma once
#include <sqlite3.h>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace mini::detail {
class Database {
public:
    sqlite3* handle{};
    Database(const std::string& path, bool writable, int cache_mib = 0) {
        if (cache_mib < 0 || cache_mib > 1024) throw std::invalid_argument("Database cache must be between 1 and 1024 MiB (0 keeps the default)");
        const int flags = writable ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY;
        const int rc = sqlite3_open_v2(path.c_str(), &handle, flags, nullptr);
        if (rc != SQLITE_OK) {
            const std::string error = handle ? sqlite3_errmsg(handle) : "out of memory";
            sqlite3_close(handle);
            throw std::runtime_error("Open index: " + error);
        }
        sqlite3_busy_timeout(handle, 5000);
        try {
            exec("PRAGMA foreign_keys=ON; PRAGMA trusted_schema=OFF;");
            if (cache_mib != 0) exec("PRAGMA cache_size=-" + std::to_string(cache_mib * 1024));
        }
        catch (...) { sqlite3_close(handle); throw; }
    }
    ~Database() { sqlite3_close(handle); }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    std::int64_t cache_stat(int operation) const noexcept {
        int current = 0, high = 0;
        if (sqlite3_db_status(handle, operation, &current, &high, 0) != SQLITE_OK) return -1;
        return current;
    }
    void exec(const std::string& sql) {
        if (sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("SQLite: " + std::string(sqlite3_errmsg(handle)));
    }
};

class Statement {
    sqlite3_stmt* stmt_{};
    Database& db_;
    void check(int rc) {
        if (rc != SQLITE_OK) throw std::runtime_error("SQLite: " + std::string(sqlite3_errmsg(db_.handle)));
    }
public:
    Statement(Database& db, const char* sql) : db_(db) {
        check(sqlite3_prepare_v2(db.handle, sql, -1, &stmt_, nullptr));
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void bind(int index, const std::string& text) {
        check(sqlite3_bind_text(stmt_, index, text.c_str(), -1, SQLITE_TRANSIENT));
    }
    void bind(int index, std::int64_t value) { check(sqlite3_bind_int64(stmt_, index, value)); }
    void null(int index) { check(sqlite3_bind_null(stmt_, index)); }
    bool row() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error("SQLite: " + std::string(sqlite3_errmsg(db_.handle)));
    }
    void run() { if (row()) throw std::runtime_error("Unexpected SQLite result"); }
    void reset() { check(sqlite3_reset(stmt_)); check(sqlite3_clear_bindings(stmt_)); }
    std::int64_t number(int column) const { return sqlite3_column_int64(stmt_, column); }
    std::string text(int column) const {
        const auto* value = sqlite3_column_text(stmt_, column);
        return value ? reinterpret_cast<const char*>(value) : "";
    }
};

class Transaction {
    Database& db_;
    bool committed_{};
public:
    explicit Transaction(Database& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }
    ~Transaction() {
        if (!committed_) sqlite3_exec(db_.handle, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit() { db_.exec("COMMIT"); committed_ = true; }
};
} // namespace mini::detail
