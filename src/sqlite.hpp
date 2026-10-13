#pragma once
#include <sqlite3.h>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace mini::detail {
class Database {
public:
    sqlite3* handle{};
    /// 按 UTF-8 路径打开只读或读写连接，create 控制读写模式是否允许创建缺失文件。
    /// cache_mib 为连接局部的页缓存目标，0 保留 SQLite 默认值；打开或配置失败时抛出异常。
    Database(const std::string& path, bool writable, int cache_mib = 0, bool create = true) {
        if (cache_mib < 0 || cache_mib > 1024) throw std::invalid_argument("Database cache must be between 1 and 1024 MiB (0 keeps the default)");
        const int flags = writable ? SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0) : SQLITE_OPEN_READONLY;
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
    /// 释放连接；使用者应让依赖该连接的语句和事务先完成析构。
    ~Database() { sqlite3_close(handle); }
    /// 禁止拷贝连接所有权，避免多个对象关闭同一个 SQLite 句柄。
    Database(const Database&) = delete;
    /// 禁止通过赋值共享连接所有权，保持连接与包装对象的一对一关系。
    Database& operator=(const Database&) = delete;
    /// 读取指定 SQLite 连接缓存计数；不抛异常，不支持该统计项时返回 -1。
    std::int64_t cache_stat(int operation) const noexcept {
        int current = 0, high = 0;
        if (sqlite3_db_status(handle, operation, &current, &high, 0) != SQLITE_OK) return -1;
        return current;
    }
    /// 执行无需读取结果集的 SQL 或 PRAGMA，失败时把 SQLite 错误转换为 C++ 异常。
    void exec(const std::string& sql) {
        if (sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("SQLite: " + std::string(sqlite3_errmsg(handle)));
    }
};

class Statement {
    sqlite3_stmt* stmt_{};
    Database& db_;
    /// 检查准备、绑定和重置操作的返回码，失败时附带当前连接的错误信息抛出异常。
    void check(int rc) {
        if (rc != SQLITE_OK) throw std::runtime_error("SQLite: " + std::string(sqlite3_errmsg(db_.handle)));
    }
public:
    /// 在 db 上预编译 SQL；调用方需保证连接的生命周期覆盖整个语句使用期。
    Statement(Database& db, const char* sql) : db_(db) {
        check(sqlite3_prepare_v2(db.handle, sql, -1, &stmt_, nullptr));
    }
    /// 释放预编译语句及其绑定资源，让所属数据库连接可以正常关闭。
    ~Statement() { sqlite3_finalize(stmt_); }
    /// 禁止复制语句句柄，避免重复 finalize 同一条预编译语句。
    Statement(const Statement&) = delete;
    /// 禁止赋值转移或共享语句句柄，保持资源所有权明确。
    Statement& operator=(const Statement&) = delete;
    /// 将文本副本绑定到从 1 开始的参数位置，SQLite 不依赖调用方字符串的后续生命周期。
    void bind(int index, const std::string& text) {
        check(sqlite3_bind_text(stmt_, index, text.c_str(), -1, SQLITE_TRANSIENT));
    }
    /// 将 64 位整数绑定到从 1 开始的参数位置，用于节点 ID、大小和时间等字段。
    void bind(int index, std::int64_t value) { check(sqlite3_bind_int64(stmt_, index, value)); }
    /// 将指定参数绑定为 SQL NULL，用于没有父节点的索引根等可空字段。
    void null(int index) { check(sqlite3_bind_null(stmt_, index)); }
    /// 推进语句执行；有当前结果行时返回 true，执行结束返回 false，SQLite 错误时抛异常。
    bool row() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error("SQLite: " + std::string(sqlite3_errmsg(db_.handle)));
    }
    /// 执行预期不返回结果行的写入语句；若实际返回行或执行失败，则抛出异常。
    void run() { if (row()) throw std::runtime_error("Unexpected SQLite result"); }
    /// 重置执行状态并清空参数绑定，以便在批量插入中复用同一条预编译语句。
    void reset() { check(sqlite3_reset(stmt_)); check(sqlite3_clear_bindings(stmt_)); }
    /// 把当前结果行中从 0 开始的指定列读取为 64 位整数。
    std::int64_t number(int column) const { return sqlite3_column_int64(stmt_, column); }
    /// 把当前结果行的指定列读取为文本副本，SQL NULL 对应空字符串；列序号从 0 开始。
    std::string text(int column) const {
        const auto* value = sqlite3_column_text(stmt_, column);
        return value ? reinterpret_cast<const char*>(value) : "";
    }
};

class Transaction {
    Database& db_;
    bool committed_{};
public:
    /// 开始 IMMEDIATE 写事务，提前获取写入保留锁；锁等待或开启失败时抛出异常。
    explicit Transaction(Database& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }
    /// 若尚未成功提交，则尽力回滚；析构中不抛异常，确保扫描失败可以保留旧索引。
    ~Transaction() {
        if (!committed_) sqlite3_exec(db_.handle, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    /// 禁止复制事务状态，避免多个对象控制同一事务的提交或回滚。
    Transaction(const Transaction&) = delete;
    /// 禁止赋值覆盖事务状态，保持事务与原连接绑定。
    Transaction& operator=(const Transaction&) = delete;
    /// 提交事务，仅在 COMMIT 成功后标记完成；失败时仍由析构流程尝试回滚。
    void commit() { db_.exec("COMMIT"); committed_ = true; }
};
} // namespace mini::detail
