#include "mini/query.hpp"
#include "../src/sqlite.hpp"
#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>

namespace fs = std::filesystem;
/// 断言真实查询结果，不满足约定时报告具体错误。
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
/// 检查操作抛出指定异常类型，防止 SQL 错误误替代参数错误。
template<class E, class F> void rejects(F action) {
    bool caught = false;
    try { action(); } catch (const E&) { caught = true; }
    require(caught, "expected exception was not thrown");
}
struct Fixture {
    fs::path base = fs::temp_directory_path() / ("mini-query-" + std::to_string(std::random_device{}()));
    fs::path root = base / "root";
    fs::path db = base / "index.db";
    /// 创建仅供本次测试使用的独立真实文件树。
    Fixture() { fs::create_directories(root); }
    /// 尽力删除本夹具，析构不会掩盖测试异常。
    ~Fixture() { std::error_code ec; fs::remove_all(base, ec); }
    /// 写入真实文件以便通过正式扫描生成 schema 1 索引。
    void file(const fs::path& name, const std::string& value = "abc") {
        fs::create_directories((root / name).parent_path());
        std::ofstream out(root / name, std::ios::binary); out << value;
        require(static_cast<bool>(out), "fixture file write failed");
    }
    /// 只修改临时数据库，用于确定性时间、深树和故障夹具。
    void sql(const std::string& command) {
        const auto path = db.u8string();
        mini::detail::Database connection(reinterpret_cast<const char*>(path.c_str()), true);
        connection.exec(command);
    }
    /// 用现有搜索获取夹具节点 ID，避免由被测新接口计算预期值。
    mini::Node node(const std::string& name) {
        const auto result = mini::search(db, {name});
        require(result.size() == 1, "fixture node must be unique"); return result.front();
    }
    /// 读取夹具根的持久化 root ID，它与节点 ID 属于独立命名空间。
    std::int64_t root_id() {
        const auto path = db.u8string();
        mini::detail::Database connection(reinterpret_cast<const char*>(path.c_str()), false);
        mini::detail::Statement statement(connection, "SELECT id FROM roots ORDER BY id LIMIT 1");
        require(statement.row(), "fixture root missing"); return statement.number(0);
    }
};

/// 捕获字面匹配、大小写及 SQL 文本注入导致的错误分支。
void matching() {
    Fixture f; f.file("Alpha.TXT"); f.file("alphabet.txt"); f.file("100%_report.txt");
    f.file("100xxreport.txt"); f.file("O'Brien.txt"); f.file(fs::path(u8"资料/年度报告.txt"));
    fs::create_directory(f.root / "folder.txt"); mini::scan(f.root, f.db);
    mini::QueryOptions o{"ALPHA.TXT"}; o.mode = mini::MatchMode::exact;
    require(mini::query(f.db, o).size() == 1, "exact must use ASCII NOCASE");
    o.text = "ALPHA"; require(mini::query(f.db, o).empty(), "exact must not match prefixes");
    o.mode = mini::MatchMode::prefix;
    require(mini::query(f.db, o).size() == 2, "prefix case matching failed");
    o.text = "100%_"; require(mini::query(f.db, o).size() == 1, "prefix wildcards not literal");
    o.mode = mini::MatchMode::substring; o.text = "%_";
    require(mini::query(f.db, o).size() == 1, "substring wildcards not literal");
    o.text = "O'Brien"; require(mini::query(f.db, o).size() == 1, "quotes not bound safely");
    o.text = "' OR 1=1 --"; require(mini::query(f.db, o).empty(), "SQL injection");
    o.text = "年度报告"; require(mini::query(f.db, o).size() == 1, "Unicode literal mismatch");
    o.text = ""; o.extension = ".TXT";
    require(mini::query(f.db, o).size() == 6, "extension must normalize case and exclude dirs");
    o.text = "资料"; o.extension = ""; o.match_path = true; o.kind = mini::NodeKind::file;
    require(mini::query(f.db, o).size() == 1, "path matching failed");
    o.text = f.node("Alpha.TXT").path; o.mode = mini::MatchMode::exact;
    require(mini::query(f.db, o).size() == 1, "exact full path mismatch");
    o.text = ""; o.mode = mini::MatchMode::substring; o.kind = mini::NodeKind::directory;
    require(mini::query(f.db, o).size() == 3, "directory kind mismatch");
    // Windows 文件名不允许反斜杠，因此通过真实 SQLite 行验证转义边界。
    f.sql("UPDATE nodes SET name='slash\\%_x' WHERE name='Alpha.TXT'");
    o = {}; o.mode = mini::MatchMode::prefix; o.text = "slash\\%_";
    require(mini::query(f.db, o).size() == 1, "backslash prefix escaping failed");
}

/// 捕获范围、大小和修改时间组合过滤失效，以及根 ID 与节点 ID 混淆。
void filtering() {
    Fixture f; f.file("docs/one.txt", "1"); f.file("docs/two.txt", "12345"); f.file("other/three.txt", "123456789");
    mini::scan(f.root, f.db); f.sql("UPDATE nodes SET modified_at=100; UPDATE nodes SET modified_at=200 WHERE name='two.txt'");
    const auto docs = f.node("docs"); const auto one = f.node("one.txt");
    const auto sibling = f.base / "sibling"; fs::create_directories(sibling);
    { std::ofstream out(sibling / "outside.txt"); out << "x"; }
    mini::scan(sibling, f.db);
    mini::QueryOptions o; o.root_id = f.root_id(); o.under_id = docs.id;
    require(mini::query(f.db, o).size() == 3, "subtree should include scope node");
    o.kind = mini::NodeKind::file; o.extension = "TXT"; o.min_size = 2; o.max_size = 5;
    o.modified_after = 200; o.modified_before = 200;
    auto result = mini::query(f.db, o);
    require(result.size() == 1 && result[0].name == "two.txt", "combined inclusive filters failed");
    o = {}; o.under_id = one.id;
    require(mini::query(f.db, o).size() == 1, "file subtree must include file itself");
    o = {}; o.kind = mini::NodeKind::directory; o.min_size = 0; o.max_size = 0;
    require(mini::query(f.db, o).size() == 4, "directory size must use stored zero");
    o = {}; o.root_id = f.root_id();
    require(mini::query(f.db, o).size() == 6, "root filtering leaked sibling root");
    o.under_id = f.node("outside.txt").id;
    require(mini::query(f.db, o).empty(), "combined unrelated scopes should have empty intersection");
}

/// 捕获字段排序、稳定分页及倒序错误；同名文件用路径破除并列。
void sorting() {
    Fixture f; f.file("a/same.txt", "12345"); f.file("b/same.txt", "1"); f.file("c/z.txt", "123");
    mini::scan(f.root, f.db);
    f.sql("UPDATE nodes SET modified_at=100; UPDATE nodes SET modified_at=200 WHERE name='z.txt'");
    mini::QueryOptions o; o.kind = mini::NodeKind::file;
    auto result = mini::query(f.db, o);
    require(result.size() == 3 && fs::path(result[0].path).parent_path().filename() == "a" &&
        fs::path(result[1].path).parent_path().filename() == "b" && result[2].name == "z.txt", "name ordering failed");
    const auto first = result[0].id, second = result[1].id;
    o.limit = 1; o.offset = 1;
    require(mini::query(f.db, o)[0].id == second, "stable pagination failed");
    o.offset = 1000000; require(mini::query(f.db, o).empty(), "large valid offset failed");
    o.offset = 0; o.limit = 100; o.sort = mini::SortField::size;
    require(mini::query(f.db, o)[0].size == 1, "size ordering failed");
    o.descending = true; require(mini::query(f.db, o)[0].size == 5, "descending size ordering failed");
    o.sort = mini::SortField::modified; require(mini::query(f.db, o)[0].name == "z.txt", "modified ordering failed");
    o.sort = mini::SortField::path; o.descending = false;
    require(mini::query(f.db, o)[0].id == first, "path ordering failed");
}

/// 捕获节点详情、直接子项、祖先方向、树深度和统计不包含自身的错误。
void browsing() {
    Fixture f; f.file("docs/sub/a.txt", "12345"); f.file("docs/b.txt", "123"); fs::create_directory(f.root / "empty");
    mini::scan(f.root, f.db); const auto docs = f.node("docs"), leaf = f.node("a.txt"), root = f.node("root");
    require(mini::get_node(f.db, leaf.id).size == 5, "detail metadata mismatch");
    const auto kids = mini::children(f.db, docs.id);
    require(kids.size() == 2 && kids[0].name == "b.txt" && kids[1].name == "sub", "children order mismatch");
    require(mini::children(f.db, docs.id, 1, 1)[0].name == "sub", "children pagination failed");
    require(mini::children(f.db, leaf.id).empty(), "file should have no children");
    const auto parents = mini::ancestors(f.db, leaf.id);
    require(parents.size() == 4 && parents.front().id == root.id && parents.back().id == leaf.id, "ancestor order must be root to self");
    auto entries = mini::tree(f.db, docs.id, 0);
    require(entries.size() == 1 && entries[0].node.id == docs.id && entries[0].depth == 0, "zero depth should include self only");
    entries = mini::tree(f.db, docs.id, 1);
    require(entries.size() == 3 && entries[1].depth == 1 && entries[2].depth == 1, "bounded tree depth failed");
    require(mini::tree(f.db, docs.id, 1000, 2).size() == 2, "tree limit failed");
    const auto stats = mini::summarize(f.db, docs.id);
    require(stats.files == 2 && stats.directories == 2 && stats.bytes == 8, "subtree stats wrong");
    const auto file_stats = mini::summarize(f.db, leaf.id);
    require(file_stats.files == 1 && file_stats.directories == 0 && file_stats.bytes == 5, "file stats must include self");
    fs::remove_all(f.root);
    require(mini::get_node(f.db, leaf.id).name == "a.txt", "browsing should support offline roots");
}

/// 捕获所有参数校验缺失，尤其非法枚举和 NUL 被 SQLite 截断。
void invalid_parameters() {
    Fixture f; f.file("a.txt"); mini::scan(f.root, f.db); const auto id = f.node("a.txt").id;
    std::vector<mini::QueryOptions> invalid;
    mini::QueryOptions o;
    o.limit = 0; invalid.push_back(o); o.limit = 10001; invalid.push_back(o);
    o = {}; o.offset = -1; invalid.push_back(o); o.offset = 1000001; invalid.push_back(o);
    o = {}; o.mode = static_cast<mini::MatchMode>(99); invalid.push_back(o);
    o = {}; o.kind = static_cast<mini::NodeKind>(99); invalid.push_back(o);
    o = {}; o.sort = static_cast<mini::SortField>(99); invalid.push_back(o);
    o = {}; o.min_size = -1; invalid.push_back(o); o = {}; o.max_size = -1; invalid.push_back(o);
    o = {}; o.min_size = 2; o.max_size = 1; invalid.push_back(o);
    o = {}; o.modified_after = 2; o.modified_before = 1; invalid.push_back(o);
    o = {}; o.root_id = -1; invalid.push_back(o); o.root_id = 999999; invalid.push_back(o);
    o = {}; o.under_id = -1; invalid.push_back(o); o.under_id = 999999; invalid.push_back(o);
    o = {}; o.text = std::string("a\0b", 3); invalid.push_back(o);
    o = {}; o.extension = std::string("t\0xt", 4); invalid.push_back(o);
    for (const auto& bad : invalid) {
        // 每组回调实际执行正式查询并要求明确参数异常。
        rejects<std::invalid_argument>([&] { mini::query(f.db, bad); });
    }
    for (const auto bad : {std::int64_t{0}, std::int64_t{-1}, std::int64_t{999999}}) {
        // 节点详情必须拒绝非法或不存在的 ID。
        rejects<std::invalid_argument>([&] { mini::get_node(f.db, bad); });
        // 子项服务必须拒绝非法或不存在的父节点 ID。
        rejects<std::invalid_argument>([&] { mini::children(f.db, bad); });
        // 祖先服务必须拒绝非法或不存在的节点 ID。
        rejects<std::invalid_argument>([&] { mini::ancestors(f.db, bad); });
        // 树服务必须拒绝非法或不存在的节点 ID。
        rejects<std::invalid_argument>([&] { mini::tree(f.db, bad); });
        // 统计服务必须拒绝非法或不存在的节点 ID。
        rejects<std::invalid_argument>([&] { mini::summarize(f.db, bad); });
    }
    // 子项分页数量不得为零。
    rejects<std::invalid_argument>([&] { mini::children(f.db, id, 0); });
    // 子项分页数量不得超过上界。
    rejects<std::invalid_argument>([&] { mini::children(f.db, id, 10001); });
    // 子项偏移不得为负数。
    rejects<std::invalid_argument>([&] { mini::children(f.db, id, 1, -1); });
    // 子项偏移不得超过上界。
    rejects<std::invalid_argument>([&] { mini::children(f.db, id, 1, 1000001); });
    // 树深度不得为负数。
    rejects<std::invalid_argument>([&] { mini::tree(f.db, id, -1); });
    // 树深度不得超过约定上限。
    rejects<std::invalid_argument>([&] { mini::tree(f.db, id, 1001); });
    // 树条数不得为零。
    rejects<std::invalid_argument>([&] { mini::tree(f.db, id, 3, 0); });
    // 树条数不得超过约定上限。
    rejects<std::invalid_argument>([&] { mini::tree(f.db, id, 3, 10001); });
    o = {}; o.modified_after = -100; o.modified_before = 0;
    require(mini::query(f.db, o).empty(), "negative Unix times should be accepted");
}

/// 捕获父链循环、缺失父节点、跨根父链和从文件继续展开等损坏索引。
void corruption() {
    Fixture f; f.file("docs/a.txt"); mini::scan(f.root, f.db); const auto docs = f.node("docs"), leaf = f.node("a.txt");
    f.sql("UPDATE nodes SET parent_id=" + std::to_string(docs.id) + " WHERE id=" + std::to_string(docs.id));
    // 祖先循环必须明确失败而非无限运行。
    rejects<std::runtime_error>([&] { mini::ancestors(f.db, leaf.id); });
    const auto bounded = mini::tree(f.db, docs.id, 1000, 100);
    require(bounded.size() <= 2, "tree cycle must not produce repeated nodes");
    mini::QueryOptions o; o.under_id = docs.id;
    require(mini::query(f.db, o).size() == 2, "subtree cycle must not duplicate nodes");
    const auto s = mini::summarize(f.db, docs.id);
    require(s.files == 1 && s.directories == 1, "cycle stats repeated nodes");
    f.sql("PRAGMA foreign_keys=OFF; UPDATE nodes SET parent_id=999999 WHERE id=" + std::to_string(docs.id));
    // 缺失父节点必须识别为索引损坏。
    rejects<std::runtime_error>([&] { mini::ancestors(f.db, leaf.id); });
    f.sql("UPDATE nodes SET parent_id=NULL WHERE id=" + std::to_string(docs.id));
    // 根内出现第二个无父节点，不应被视为健康祖先链。
    rejects<std::runtime_error>([&] { mini::ancestors(f.db, leaf.id); });
    const auto sibling = f.base / "sibling";
    fs::create_directory(sibling);
    { std::ofstream out(sibling / "outside.txt"); out << "x"; }
    mini::scan(sibling, f.db);
    const auto outside = f.node("outside.txt");
    const auto sibling_node = f.node("sibling");
    f.sql("UPDATE nodes SET parent_id=" + std::to_string(sibling_node.id) + " WHERE id=" + std::to_string(docs.id));
    // 父目录存在但属于另一登记根时，仍须拒绝祖先链。
    rejects<std::runtime_error>([&] { mini::ancestors(f.db, leaf.id); });
    f.sql("UPDATE nodes SET parent_id=" + std::to_string(outside.id) + " WHERE id=" + std::to_string(docs.id));
    // 普通文件不能作为父目录。
    rejects<std::runtime_error>([&] { mini::ancestors(f.db, leaf.id); });
}

/// 捕获无条数约束的递归展开；深树只要求前缀，同时支持最大深度一千。
void deep_and_wide() {
    Fixture f; mini::scan(f.root, f.db); const auto root = f.node("root");
    f.sql("WITH RECURSIVE seq(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM seq WHERE x<1200) "
          "INSERT INTO nodes(id,root_id,parent_id,name,full_path,extension,is_directory,size,modified_at) "
          "SELECT x+10000,1,CASE WHEN x=1 THEN " + std::to_string(root.id) + " ELSE x+9999 END,'d', 'deep/'||x,'',1,0,0 FROM seq");
    require(mini::tree(f.db, root.id, 1000, 2).size() == 2, "deep tree prefix failed");
    const auto entries = mini::tree(f.db, root.id, 1000, 10000);
    require(entries.size() == 1001 && entries.back().depth == 1000, "max depth boundary failed");
    const auto stats = mini::summarize(f.db, root.id);
    require(stats.directories == 1201 && stats.files == 0 && stats.bytes == 0, "full deep stats failed");
    f.sql("WITH RECURSIVE seq(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM seq WHERE x<20000) "
          "INSERT INTO nodes(id,root_id,parent_id,name,full_path,extension,is_directory,size,modified_at) "
          "SELECT x+30000,1," + std::to_string(root.id) + ",'wide','wide/'||x,'',0,1,0 FROM seq");
    require(mini::tree(f.db, root.id, 3, 3).size() == 3, "wide tree limit failed");
}

/// 捕获只读接口对错误格式的疏漏及不存在数据库被创建的副作用。
void read_safety() {
    Fixture f;
    // 缺失数据库只允许报错，不能创建空索引。
    rejects<std::runtime_error>([&] { mini::query(f.db, {}); });
    require(!fs::exists(f.db), "query created absent database");
    f.sql("CREATE TABLE unrelated(value TEXT)");
    // 外部 SQLite 文件不得作为索引读取。
    rejects<std::runtime_error>([&] { mini::query(f.db, {}); });
    fs::remove(f.db); mini::scan(f.root, f.db);
    require(mini::query(f.db, {}).size() == 1, "empty file root should remain browsable");
    f.sql("PRAGMA user_version=99");
    // 所有新入口共用 schema 校验。
    rejects<std::runtime_error>([&] { mini::get_node(f.db, 1); });
    f.sql("PRAGMA user_version=1");
    const auto bytes = fs::file_size(f.db);
    mini::query(f.db, {}); mini::tree(f.db, 1); mini::summarize(f.db, 1);
    require(fs::file_size(f.db) == bytes, "read operations changed database");
    f.sql("DELETE FROM nodes; DELETE FROM roots");
    require(mini::query(f.db, {}).empty(), "empty registered-root catalog should return no results");
    // 空索引中的旧节点 ID 不能返回默认节点。
    rejects<std::invalid_argument>([&] { mini::get_node(f.db, 1); });
}

/// 单一无参数入口运行全部查询场景，供独立验证和 CTest 注册。
int main() {
    const std::pair<const char*, void(*)()> cases[] = {
        {"matching", matching}, {"filtering", filtering}, {"sorting", sorting},
        {"browsing", browsing}, {"invalid_parameters", invalid_parameters},
        {"corruption", corruption}, {"deep_and_wide", deep_and_wide}, {"read_safety", read_safety}
    };
    int failures = 0;
    for (const auto& [name, test] : cases) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    return failures == 0 ? 0 : 1;
}
