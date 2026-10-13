#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mini {
struct Node {
    std::int64_t id{};
    std::int64_t parent_id{}; // 0 means index root
    std::string name;
    std::string path;
    bool is_directory{};
    std::int64_t size{};
    std::int64_t modified_at{}; // Unix seconds
};

struct ScanTimings {
    double prepare_ms{}; // database open, schema and previous-root cleanup
    double open_ms{};
    double transaction_ms{}; // includes waiting for the write lock
    double schema_ms{};
    double roots_ms{};
    double clear_ms{};
    double statements_ms{};
    double metadata_ms{}; // enumeration and metadata
    double identity_ms{};
    double write_ms{}; // SQL and serialization
    double commit_ms{};
};
struct ScanDatabaseStats {
    int cache_mib{}; // requested page-cache target, not total process memory
    std::int64_t cache_hits{};
    std::int64_t cache_misses{};
    std::int64_t cache_writes{};
    std::int64_t cache_spills{};
};
struct ScanResult {
    std::int64_t files{};
    std::int64_t directories{}; // includes root
    std::int64_t skipped{}; // links / unsupported file types
    std::int64_t identity_checks{}; // per-file identity comparisons
    ScanTimings timings;
    ScanDatabaseStats database_stats;
    std::int64_t inserted{}; // changed node rows, including directories
    std::int64_t updated{};
    std::int64_t removed{};
    std::int64_t unchanged{};
};
enum class ScanPhase { preparing, scanning, committing };
struct ScanProgress {
    ScanPhase phase{};
    std::int64_t files{};
    std::int64_t directories{};
    std::int64_t skipped{};
};
class ScanCancelled : public std::runtime_error {
public:
    /// 标记提交前扫描取消，已有索引由当前事务回滚保留。
    ScanCancelled() : std::runtime_error("Scan cancelled; index changes rolled back") {}
};
struct ScanOptions {
    int db_cache_mib{64}; // 1..1024, connection-local
    bool incremental{false}; // preserve IDs of paths that remain present
    std::int64_t expected_root_id{}; // 0 unrestricted; >0 requires this registered root
    std::function<bool()> cancelled{};
    std::function<void(const ScanProgress&)> progress{};
    int progress_interval_ms{1000}; // 0 disables throttling; negative values invalid
};

struct SearchOptions {
    std::string text;
    std::string extension{}; // optional; leading dot accepted
    int limit{100}; // 1..10000
};

// Paths at the public boundary are native filesystem paths; stored text is UTF-8.
/// 扫描 root 并在单个事务中重建或增量校准该根，返回数量、变化、耗时和缓存统计。
/// database 必须位于扫描范围外；取消、回调或写入失败时回滚，进度回调只在提交前运行。
ScanResult scan(const std::filesystem::path& root, const std::filesystem::path& database, const ScanOptions& options = {});
/// 从已有索引中按文件名的字面子串搜索，应用扩展名过滤和数量限制后返回节点。
/// 只读数据库，不遍历实际目录；数据库格式或查询参数不合法时抛出异常。
std::vector<Node> search(const std::filesystem::path& database, const SearchOptions& options);
} // namespace mini
