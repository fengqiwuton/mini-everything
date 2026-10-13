#include "mini/query.hpp"
#include "read_index.hpp"
#include <algorithm>
#include <unordered_set>
#include <variant>

namespace mini {
namespace {
using detail::Database;
using detail::Statement;
using Parameter = std::variant<std::int64_t, std::string>;
constexpr const char* columns = "n.id,coalesce(n.parent_id,0),n.name,n.full_path,n.is_directory,n.size,n.modified_at";
constexpr const char* scope_sql =
    "WITH RECURSIVE scope(id) AS (SELECT id FROM nodes WHERE id=? "
    "UNION SELECT n.id FROM nodes n JOIN scope s ON n.parent_id=s.id) ";

/// 校验分页上下界，统一查询和直接子项的参数行为。
void validate_page(int limit, int offset) {
    if (limit < 1 || limit > 10000) throw std::invalid_argument("Limit must be between 1 and 10000");
    if (offset < 0 || offset > 1000000) throw std::invalid_argument("Offset must be between 0 and 1000000");
}
/// 校验节点 ID 为正数，不把范围过滤中的零约定应用到单节点操作。
void validate_id(std::int64_t id) {
    if (id <= 0) throw std::invalid_argument("Node ID must be positive");
}
/// 将当前行中的七个持久化字段转换为公开节点结构。
Node read_node(const Statement& statement) {
    return {statement.number(0), statement.number(1), statement.text(2), statement.text(3),
        statement.number(4) != 0, statement.number(5), statement.number(6)};
}
struct StoredNode { Node node; std::int64_t root_id{}; };
/// 在同一读事务内读取节点及所属根；调用方选择用户错误或索引损坏错误。
StoredNode lookup(Database& db, std::int64_t id, bool parent = false) {
    const std::string sql = std::string("SELECT ") + columns + ",n.root_id FROM nodes n WHERE n.id=?";
    Statement statement(db, sql.c_str()); statement.bind(1, id);
    if (!statement.row()) {
        if (parent) throw std::runtime_error("Index parent node is missing");
        throw std::invalid_argument("Node ID does not exist");
    }
    return {read_node(statement), statement.number(7)};
}
/// 查询根的登记路径；范围筛选缺失根为参数错误，损坏父链缺失根为运行错误。
std::string root_path(Database& db, std::int64_t id, bool chain = false) {
    Statement statement(db, "SELECT path FROM roots WHERE id=?"); statement.bind(1, id);
    if (!statement.row()) {
        if (chain) throw std::runtime_error("Index root record is missing");
        throw std::invalid_argument("Root ID does not exist");
    }
    return statement.text(0);
}
/// 将用户文本转换为 LIKE 字面模式，转义反斜杠、百分号和下划线。
std::string like_pattern(const std::string& text, MatchMode mode) {
    std::string result;
    if (mode == MatchMode::substring) result += '%';
    for (const char c : text) {
        if (c == '\\' || c == '%' || c == '_') result += '\\';
        result += c;
    }
    result += '%';
    return result;
}
/// 校验枚举、文本及数值范围；负数 Unix 时间可表达 1970 年之前的时间。
void validate_options(const QueryOptions& options) {
    validate_page(options.limit, options.offset);
    switch (options.mode) {
    case MatchMode::substring: case MatchMode::exact: case MatchMode::prefix: break;
    default: throw std::invalid_argument("Invalid match mode");
    }
    switch (options.kind) {
    case NodeKind::any: case NodeKind::file: case NodeKind::directory: break;
    default: throw std::invalid_argument("Invalid node kind");
    }
    switch (options.sort) {
    case SortField::name: case SortField::path: case SortField::size: case SortField::modified: break;
    default: throw std::invalid_argument("Invalid sort field");
    }
    if (options.text.find('\0') != std::string::npos || options.extension.find('\0') != std::string::npos)
        throw std::invalid_argument("Query text must not contain NUL");
    if (options.root_id < 0 || options.under_id < 0) throw std::invalid_argument("Scope IDs must not be negative");
    if ((options.min_size && *options.min_size < 0) || (options.max_size && *options.max_size < 0))
        throw std::invalid_argument("Size bounds must not be negative");
    if (options.min_size && options.max_size && *options.min_size > *options.max_size)
        throw std::invalid_argument("Minimum size exceeds maximum size");
    if (options.modified_after && options.modified_before && *options.modified_after > *options.modified_before)
        throw std::invalid_argument("Modified lower bound exceeds upper bound");
}
/// 绑定查询动态条件的参数，用户输入只进入参数槽位。
void bind_parameters(Statement& statement, const std::vector<Parameter>& parameters) {
    int index = 1;
    for (const auto& value : parameters) {
        if (const auto* number = std::get_if<std::int64_t>(&value)) statement.bind(index, *number);
        else statement.bind(index, std::get<std::string>(value));
        ++index;
    }
}
/// 返回排序字段的固定 SQL 片段，避免任何用户字段名直接进入 SQL。
std::string sort_column(SortField sort) {
    switch (sort) {
    case SortField::name: return "n.name COLLATE NOCASE";
    case SortField::path: return "n.full_path";
    case SortField::size: return "n.size";
    case SortField::modified: return "n.modified_at";
    }
    throw std::invalid_argument("Invalid sort field");
}
} // namespace

/// 在只读快照中动态组合可索引的文本条件、范围与元数据条件，稳定排序并分页。
std::vector<Node> query(const std::filesystem::path& database, const QueryOptions& options) {
    validate_options(options);
    detail::ReadIndex index(database);
    if (options.root_id != 0) root_path(index.db, options.root_id);
    if (options.under_id != 0) lookup(index.db, options.under_id);
    std::string sql;
    std::vector<Parameter> parameters;
    if (options.under_id != 0) { sql += scope_sql; parameters.emplace_back(options.under_id); }
    sql += std::string("SELECT ") + columns + " FROM nodes n WHERE ";
    const std::string field = options.match_path ? "n.full_path" : "n.name";
    if (options.mode == MatchMode::exact) {
        sql += field + " = ? COLLATE NOCASE"; parameters.emplace_back(options.text);
    } else {
        sql += field + " LIKE ? ESCAPE '\\'"; parameters.emplace_back(like_pattern(options.text, options.mode));
    }
    auto extension = detail::ascii_lower(options.extension);
    if (!extension.empty()) {
        if (extension.front() != '.') extension.insert(extension.begin(), '.');
        sql += " AND n.is_directory=0 AND n.extension=?"; parameters.emplace_back(extension);
    }
    if (options.kind != NodeKind::any) {
        sql += " AND n.is_directory=?";
        parameters.emplace_back(static_cast<std::int64_t>(options.kind == NodeKind::directory));
    }
    if (options.root_id != 0) { sql += " AND n.root_id=?"; parameters.emplace_back(options.root_id); }
    if (options.under_id != 0) sql += " AND n.id IN (SELECT id FROM scope)";
    // 每个可选边界均直接生成固定比较条件，保留 SQLite 对过滤列的优化空间。
    const auto add_bound = [&](const char* condition, const std::optional<std::int64_t>& bound) {
        if (bound) { sql += condition; parameters.emplace_back(*bound); }
    };
    add_bound(" AND n.size>=?", options.min_size); add_bound(" AND n.size<=?", options.max_size);
    add_bound(" AND n.modified_at>=?", options.modified_after); add_bound(" AND n.modified_at<=?", options.modified_before);
    sql += " ORDER BY " + sort_column(options.sort) + (options.descending ? " DESC" : " ASC") + ",n.full_path,n.id LIMIT ? OFFSET ?";
    parameters.emplace_back(static_cast<std::int64_t>(options.limit)); parameters.emplace_back(static_cast<std::int64_t>(options.offset));
    Statement statement(index.db, sql.c_str()); bind_parameters(statement, parameters);
    std::vector<Node> result;
    while (statement.row()) result.push_back(read_node(statement));
    return result;
}

/// 在只读快照中返回正数节点 ID 的详情。
Node get_node(const std::filesystem::path& database, std::int64_t id) {
    validate_id(id); detail::ReadIndex index(database); return lookup(index.db, id).node;
}

/// 确认父节点存在后，使用父 ID 索引返回按名称稳定分页的直接子项。
std::vector<Node> children(const std::filesystem::path& database, std::int64_t id, int limit, int offset) {
    validate_id(id); validate_page(limit, offset); detail::ReadIndex index(database); lookup(index.db, id);
    const std::string sql = std::string("SELECT ") + columns +
        " FROM nodes n WHERE n.parent_id=? ORDER BY n.name COLLATE NOCASE,n.full_path,n.id LIMIT ? OFFSET ?";
    Statement statement(index.db, sql.c_str()); statement.bind(1, id);
    statement.bind(2, static_cast<std::int64_t>(limit)); statement.bind(3, static_cast<std::int64_t>(offset));
    std::vector<Node> result;
    while (statement.row()) result.push_back(read_node(statement));
    return result;
}

/// 逐级读取同一根内的目录父节点并检测循环，最终核对登记根路径后反转为根到自身。
std::vector<Node> ancestors(const std::filesystem::path& database, std::int64_t id) {
    validate_id(id); detail::ReadIndex index(database); auto current = lookup(index.db, id);
    const auto expected_root = current.root_id; const auto registered_path = root_path(index.db, expected_root, true);
    std::unordered_set<std::int64_t> visited;
    std::vector<Node> result;
    for (;;) {
        if (!visited.insert(current.node.id).second) throw std::runtime_error("Index parent chain contains a cycle");
        if (current.root_id != expected_root) throw std::runtime_error("Index parent chain crosses root boundaries");
        result.push_back(current.node);
        if (current.node.parent_id == 0) {
            if (!current.node.is_directory || current.node.path != registered_path)
                throw std::runtime_error("Index parent chain does not end at its registered root");
            break;
        }
        current = lookup(index.db, current.node.parent_id, true);
        if (!current.node.is_directory) throw std::runtime_error("Index parent node is not a directory");
    }
    std::reverse(result.begin(), result.end()); return result;
}

/// 在递归 CTE 内限制条数与深度，路径中的 ID 集合阻止循环重复，输出广度优先稳定顺序。
std::vector<TreeEntry> tree(const std::filesystem::path& database, std::int64_t id, int max_depth, int limit) {
    validate_id(id); validate_page(limit, 0);
    if (max_depth < 0 || max_depth > 1000) throw std::invalid_argument("Tree depth must be between 0 and 1000");
    detail::ReadIndex index(database); lookup(index.db, id);
    const std::string sql = std::string(
        "WITH RECURSIVE walk(id,depth,trail,name,path) AS ("
        "SELECT id,0,','||id||',',name,full_path FROM nodes WHERE id=? "
        "UNION ALL SELECT n.id,w.depth+1,w.trail||n.id||',',n.name,n.full_path "
        "FROM nodes n JOIN walk w ON n.parent_id=w.id "
        "WHERE w.depth<? AND instr(w.trail,','||n.id||',')=0 "
        "ORDER BY 2,4 COLLATE NOCASE,5,1 LIMIT ?) SELECT ") + columns +
        ",w.depth FROM walk w JOIN nodes n ON n.id=w.id ORDER BY w.depth,w.name COLLATE NOCASE,w.path,w.id";
    Statement statement(index.db, sql.c_str()); statement.bind(1, id);
    statement.bind(2, static_cast<std::int64_t>(max_depth)); statement.bind(3, static_cast<std::int64_t>(limit));
    std::vector<TreeEntry> result;
    while (statement.row()) result.push_back({read_node(statement), static_cast<int>(statement.number(7))});
    return result;
}

/// 以 UNION 去重的 ID 递归集合汇总子树，目录自身计入目录数，文件自身计入字节。
SubtreeStats summarize(const std::filesystem::path& database, std::int64_t id) {
    validate_id(id); detail::ReadIndex index(database); lookup(index.db, id);
    const std::string sql = std::string(scope_sql) +
        "SELECT coalesce(sum(n.is_directory=0),0),coalesce(sum(n.is_directory<>0),0),"
        "coalesce(sum(CASE WHEN n.is_directory=0 THEN n.size ELSE 0 END),0) FROM nodes n JOIN scope s ON n.id=s.id";
    Statement statement(index.db, sql.c_str()); statement.bind(1, id);
    if (!statement.row()) throw std::runtime_error("Index subtree statistics returned no row");
    return {statement.number(0), statement.number(1), statement.number(2)};
}
} // namespace mini
