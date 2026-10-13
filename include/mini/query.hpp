#pragma once
#include "mini/index.hpp"
#include <optional>

namespace mini {
enum class MatchMode { substring, exact, prefix };
enum class NodeKind { any, file, directory };
enum class SortField { name, path, size, modified };

struct QueryOptions {
    std::string text;
    std::string extension{};
    int limit{100};
    MatchMode mode{MatchMode::substring};
    NodeKind kind{NodeKind::any};
    SortField sort{SortField::name};
    bool descending{false};
    int offset{0};
    std::optional<std::int64_t> min_size{}, max_size{}, modified_after{}, modified_before{};
    std::int64_t root_id{0};
    std::int64_t under_id{0};
    bool match_path{false};
};
struct TreeEntry { Node node; int depth{}; };
struct SubtreeStats { std::int64_t files{}, directories{}, bytes{}; };

/// 按字面名称或路径、范围及元数据过滤已有索引，稳定排序并分页；不访问被索引文件。
std::vector<Node> query(const std::filesystem::path& database, const QueryOptions& options);
/// 返回指定正数节点 ID 的完整信息，不存在或 ID 非法时抛出 invalid_argument。
Node get_node(const std::filesystem::path& database, std::int64_t id);
/// 按名称和路径稳定返回直接子项，limit 为 1..10000，offset 为 0..1000000。
std::vector<Node> children(const std::filesystem::path& database, std::int64_t id, int limit = 100, int offset = 0);
/// 返回根到自身的祖先链；循环、缺失父节点或异常根关系抛出 runtime_error。
std::vector<Node> ancestors(const std::filesystem::path& database, std::int64_t id);
/// 返回含自身的有限树，深度 0..1000、条数 1..10000，递归扩展本身受条数限制。
std::vector<TreeEntry> tree(const std::filesystem::path& database, std::int64_t id, int max_depth = 3, int limit = 1000);
/// 统计含自身的子树文件、目录与文件总字节，循环中的节点最多统计一次。
SubtreeStats summarize(const std::filesystem::path& database, std::int64_t id);
} // namespace mini
