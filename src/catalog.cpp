#include "mini/catalog.hpp"
#include "read_index.hpp"

namespace mini {
namespace {
using namespace detail;

/// 执行单行整数查询；聚合计算由 SQLite 的 int64 SUM 检测溢出并明确抛错。
std::int64_t scalar(Database& db, const char* sql) {
    Statement query(db,sql);
    if (!query.row()) throw std::runtime_error("Missing index statistics");
    return query.number(0);
}

/// 查询某一类异常的数量，存在异常时仅添加类别说明，避免输出所有节点路径。
void check_relation(Database& db, IndexHealth& health, const char* sql, const char* issue) {
    const auto count=scalar(db,sql);
    if (count) health.issues.push_back(std::string(issue) + " (" + std::to_string(count) + ")");
}
} // namespace

/// 聚合每个根的全部节点，在同一只读快照中返回独立的根 ID 和根节点 ID。
std::vector<CatalogRoot> list_roots(const std::filesystem::path& database) {
    detail::ReadIndex index(database);
    detail::Statement query(index.db,R"sql(
        SELECT r.id,r.path,r.scanned_at,
            coalesce(min(CASE WHEN n.parent_id IS NULL THEN n.id END),0),
            coalesce(sum(CASE WHEN n.is_directory=0 THEN 1 ELSE 0 END),0),
            coalesce(sum(CASE WHEN n.is_directory=1 THEN 1 ELSE 0 END),0),
            coalesce(sum(CASE WHEN n.is_directory=0 THEN n.size ELSE 0 END),0)
        FROM roots r LEFT JOIN nodes n ON n.root_id=r.id
        GROUP BY r.id,r.path,r.scanned_at ORDER BY r.path COLLATE BINARY,r.id
    )sql");
    std::vector<CatalogRoot> result;
    while (query.row()) result.push_back({query.number(0),query.text(1),query.number(2),query.number(3),
        query.number(4),query.number(5),query.number(6)});
    return result;
}

/// 分别统计根表与节点表，文件大小使用整数 SUM，空库各项均为零。
IndexStats index_stats(const std::filesystem::path& database) {
    detail::ReadIndex index(database);
    IndexStats result;
    result.roots=scalar(index.db,"SELECT count(*) FROM roots");
    detail::Statement query(index.db,R"sql(
        SELECT coalesce(sum(CASE WHEN is_directory=0 THEN 1 ELSE 0 END),0),
            coalesce(sum(CASE WHEN is_directory=1 THEN 1 ELSE 0 END),0),
            coalesce(sum(CASE WHEN is_directory=0 THEN size ELSE 0 END),0)
        FROM nodes
    )sql");
    if (!query.row()) throw std::runtime_error("Missing index statistics");
    result.files=query.number(0); result.directories=query.number(1); result.bytes=query.number(2);
    return result;
}

/// 获得写锁后校验格式和根身份，先删除该根节点再删除根记录，最后原子提交。
void forget_root(const std::filesystem::path& database, std::int64_t root_id) {
    using namespace detail;
    if (root_id<=0) throw std::invalid_argument("Root ID must be positive");
    Database db(utf8(normalized(database)),true,64,false);
    Transaction transaction(db);
    require_index(db);
    {
        Statement root(db,"SELECT 1 FROM roots WHERE id=?");
        root.bind(1,root_id);
        if (!root.row()) throw std::invalid_argument("Root ID is not registered");
    }
    Statement nodes(db,"DELETE FROM nodes WHERE root_id=?");
    nodes.bind(1,root_id); nodes.run();
    Statement root(db,"DELETE FROM roots WHERE id=?");
    root.bind(1,root_id); root.run();
    transaction.commit();
}

/// 在一个只读快照中校验物理完整性、外键和逻辑树，用去重遍历限制循环影响。
IndexHealth check_index(const std::filesystem::path& database) {
    using namespace detail;
    ReadIndex index(database);
    IndexHealth health;
    {
        Statement integrity(index.db,"PRAGMA integrity_check");
        while (integrity.row()) {
            const auto message=integrity.text(0);
            if (message!="ok") health.issues.push_back("SQLite integrity: " + message);
        }
    }
    {
        Statement foreign_keys(index.db,"PRAGMA foreign_key_check");
        std::int64_t failures=0;
        while (foreign_keys.row()) ++failures;
        if (failures) health.issues.push_back("Foreign key violations (" + std::to_string(failures) + ")");
    }
    check_relation(index.db,health,R"sql(
        SELECT count(*) FROM nodes
        WHERE typeof(size)<>'integer' OR size<0
            OR typeof(is_directory)<>'integer' OR is_directory NOT IN (0,1)
    )sql","Nodes with invalid size or type metadata");
    check_relation(index.db,health,R"sql(
        SELECT count(*) FROM (
            SELECT r.id FROM roots r LEFT JOIN nodes n ON n.root_id=r.id AND n.parent_id IS NULL
            GROUP BY r.id HAVING count(n.id)<>1
        )
    )sql","Registered roots without exactly one root node");
    check_relation(index.db,health,R"sql(
        SELECT count(*) FROM nodes n LEFT JOIN roots r ON r.id=n.root_id
        WHERE n.parent_id IS NULL AND (r.id IS NULL OR n.full_path<>r.path OR n.is_directory<>1)
    )sql","Root nodes with invalid root identity, path or type");
    check_relation(index.db,health,R"sql(
        SELECT count(*) FROM nodes n JOIN nodes p ON p.id=n.parent_id WHERE p.root_id<>n.root_id
    )sql","Nodes with a parent in another root");
    check_relation(index.db,health,R"sql(
        SELECT count(*) FROM nodes n JOIN nodes p ON p.id=n.parent_id WHERE p.is_directory<>1
    )sql","Nodes with a non-directory parent");
    check_relation(index.db,health,R"sql(
        WITH RECURSIVE reachable(id) AS (
            SELECT id FROM nodes WHERE parent_id IS NULL
            UNION
            SELECT n.id FROM nodes n JOIN reachable p ON n.parent_id=p.id
        )
        SELECT count(*) FROM nodes WHERE id NOT IN (SELECT id FROM reachable)
    )sql","Nodes unreachable from root nodes (broken chain or cycle)");
    health.ok=health.issues.empty();
    return health;
}
} // namespace mini
