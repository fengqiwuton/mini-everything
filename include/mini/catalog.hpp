#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mini {
struct CatalogRoot {
    std::int64_t id{};
    std::string path;
    std::int64_t scanned_at{};
    std::int64_t node_id{};
    std::int64_t files{};
    std::int64_t directories{};
    std::int64_t bytes{};
};
struct IndexStats {
    std::int64_t roots{};
    std::int64_t files{};
    std::int64_t directories{};
    std::int64_t bytes{};
};
struct IndexHealth {
    bool ok{};
    std::vector<std::string> issues;
};

/// 只读列出注册根及其节点统计，按路径稳定排序；目录计数包含根，字节仅累计文件。
std::vector<CatalogRoot> list_roots(const std::filesystem::path& database);
/// 在同一只读快照中返回根、文件、目录和文件字节总数；整数溢出时明确报错。
IndexStats index_stats(const std::filesystem::path& database);
/// 在单个写事务中移除指定持久化根 ID 的索引记录，不访问该根的磁盘文件。
/// ID 必须大于零且已注册；数据库或 SQL 失败时回滚全部修改。
void forget_root(const std::filesystem::path& database, std::int64_t root_id);
/// 只读检查 SQLite 完整性、外键及根与父链关系，异常项目以有限的类别文字返回。
/// 不兼容格式和无法查询的损坏库抛出异常，不执行修复。
IndexHealth check_index(const std::filesystem::path& database);
} // namespace mini
