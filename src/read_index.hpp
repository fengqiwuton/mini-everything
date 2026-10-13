#pragma once
#include "paths.hpp"
#include "sqlite.hpp"

namespace mini::detail {
/// 校验只读/管理服务所需的应用标记与 schema 1 版本，不创建数据库或修复其内容。
inline void require_index(Database& db) {
    Statement application(db, "PRAGMA application_id");
    Statement version(db, "PRAGMA user_version");
    if (!application.row() || !version.row() || application.number(0) != 0x4d454958 || version.number(0) != 1)
        throw std::runtime_error("Not a supported MiniEverything index (schema version 1 required)");
}

class ReadIndex {
public:
    Database db;
    /// 打开规范 UTF-8 路径的只读连接，建立读事务并校验格式，让连续查询使用同一快照。
    explicit ReadIndex(const fs::path& path) : db(utf8(normalized(path)), false) {
        db.exec("BEGIN");
        try { require_index(db); }
        catch (...) { sqlite3_exec(db.handle, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
    }
    /// 结束只读事务并释放读取锁；ROLLBACK 不修改数据库，析构中不传播错误。
    ~ReadIndex() { sqlite3_exec(db.handle, "ROLLBACK", nullptr, nullptr, nullptr); }
    /// 禁止复制读连接与快照所有权。
    ReadIndex(const ReadIndex&) = delete;
    /// 禁止赋值覆盖读事务状态。
    ReadIndex& operator=(const ReadIndex&) = delete;
};
} // namespace mini::detail
