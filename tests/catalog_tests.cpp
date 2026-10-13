#include "mini/catalog.hpp"
#include "../src/paths.hpp"
#include "../src/sqlite.hpp"
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <random>

namespace fs = std::filesystem;
using mini::detail::Database;
using mini::detail::Statement;

/// 检查可观察行为；失败时让测试入口打印具体断言。
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
/// 验证非法输入或数据库故障会被明确拒绝，而非静默成功。
template<class F> void rejects(F action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "expected explicit rejection");
}

struct Fixture {
    fs::path base = fs::temp_directory_path() / ("mini-catalog-" + std::to_string(std::random_device{}()));
    fs::path db = base / fs::path(u8"目录索引.db");
    /// 创建随机临时库，沿用 schema 1 的约束，避免访问实际用户索引。
    Fixture() {
        fs::create_directories(base);
        sql(R"sql(
            PRAGMA application_id=1296386392; PRAGMA user_version=1;
            CREATE TABLE roots(id INTEGER PRIMARY KEY,path TEXT NOT NULL UNIQUE,scanned_at INTEGER NOT NULL DEFAULT 0);
            CREATE TABLE nodes(id INTEGER PRIMARY KEY,root_id INTEGER NOT NULL REFERENCES roots(id),
                parent_id INTEGER REFERENCES nodes(id),name TEXT NOT NULL,full_path TEXT NOT NULL UNIQUE,
                extension TEXT NOT NULL,is_directory INTEGER NOT NULL CHECK(is_directory IN (0,1)),
                size INTEGER NOT NULL CHECK(size>=0),modified_at INTEGER NOT NULL);
            CREATE INDEX idx_nodes_root ON nodes(root_id);
            CREATE INDEX idx_nodes_parent ON nodes(parent_id);
        )sql");
    }
    /// 仅清理本随机夹具目录，析构时不传播清理失败。
    ~Fixture() { std::error_code ec; fs::remove_all(base, ec); }
    /// 在夹具数据库上执行布置 SQL；允许制造外键损坏供健康检查验证。
    void sql(const char* text) {
        Database connection(mini::detail::utf8(db), true);
        connection.exec("PRAGMA foreign_keys=OFF");
        connection.exec(text);
    }
    /// 绑定根的持久化 ID 和离线路径，节点 ID 由测试单独指定。
    void root(std::int64_t id, const std::string& path, std::int64_t stamp = 123) {
        Database connection(mini::detail::utf8(db), true);
        Statement insert(connection, "INSERT INTO roots VALUES(?,?,?)");
        insert.bind(1,id); insert.bind(2,path); insert.bind(3,stamp); insert.run();
    }
    /// 插入指定父链的目录或文件，大小期望由测试手工给定。
    void node(std::int64_t id, std::int64_t root_id, std::int64_t parent,
              const std::string& path, bool directory, std::int64_t size = 0) {
        Database connection(mini::detail::utf8(db), true);
        Statement insert(connection, "INSERT INTO nodes VALUES(?,?,?,?,?,'',?,?,456)");
        insert.bind(1,id); insert.bind(2,root_id);
        if (parent) insert.bind(3,parent); else insert.null(3);
        insert.bind(4,path); insert.bind(5,path);
        insert.bind(6,static_cast<std::int64_t>(directory)); insert.bind(7,size); insert.run();
    }
    /// 创建两个根及五个节点，根 ID 与节点 ID 不同，目录大小不计入文件字节。
    void two_roots() {
        root(7,"Z:/offline/z",321); root(42,"A:/offline/a",654);
        node(101,7,0,"Z:/offline/z",true,100);
        node(102,7,101,"Z:/offline/z/data",false,11);
        node(900,42,0,"A:/offline/a",true,200);
        node(901,42,900,"A:/offline/a/sub",true,300);
        node(902,42,901,"A:/offline/a/sub/data",false,17);
    }
};

/// 空格式库应返回零统计和健康结果，读取缺失文件不得创建数据库。
void empty_catalog() {
    Fixture f;
    require(mini::list_roots(f.db).empty(),"empty roots not empty");
    const auto stats = mini::index_stats(f.db);
    require(stats.roots==0 && stats.files==0 && stats.directories==0 && stats.bytes==0,"empty totals incorrect");
    const auto health = mini::check_index(f.db);
    require(health.ok && health.issues.empty(),"empty formatted index unhealthy");
    const auto missing = f.base / "missing.db";
    rejects([&]{ mini::list_roots(missing); });
    require(!fs::exists(missing),"read created absent database");
}
/// forget 缺失数据库必须明确拒绝，不能在错误路径上创建空数据库。
void forget_missing_database() {
    Fixture f;
    const auto missing = f.base / "missing-forget.db";
    rejects([&]{ mini::forget_root(missing,1); });
    require(!fs::exists(missing),"forget created absent database");
}
/// 根列表必须按路径稳定排序，分别报告持久化根 ID、根节点 ID 与含根目录统计。
void multi_root_stats() {
    Fixture f; f.two_roots();
    const auto roots = mini::list_roots(f.db);
    require(roots.size()==2 && roots[0].id==42 && roots[1].id==7,"roots are not stably path sorted");
    require(roots[0].node_id==900 && roots[0].path=="A:/offline/a" && roots[0].scanned_at==654,"root identity metadata incorrect");
    require(roots[0].files==1 && roots[0].directories==2 && roots[0].bytes==17,"first root totals incorrect");
    require(roots[1].node_id==101 && roots[1].files==1 && roots[1].directories==1 && roots[1].bytes==11,"second root totals incorrect");
    const auto stats = mini::index_stats(f.db);
    require(stats.roots==2 && stats.files==2 && stats.directories==3 && stats.bytes==28,"overall totals incorrect");
    require(mini::check_index(f.db).ok,"valid multi-root index unhealthy");
}
/// forget 仅移除所选根的索引，保留其他根与磁盘文件，并明确拒绝不存在的 ID。
void forget_preserves_files() {
    Fixture f;
    f.root(7,"Z:/offline/z"); f.node(101,7,0,"Z:/offline/z",true);
    f.node(102,7,101,"Z:/offline/z/data",false,11);
    const auto directory = f.base / "forgotten";
    fs::create_directories(directory);
    const auto file = directory / "keep.txt";
    { std::ofstream out(file); out << "retained"; }
    const auto root_path=mini::detail::utf8(mini::detail::normalized(directory));
    f.root(42,root_path); f.node(900,42,0,root_path,true);
    f.node(901,42,900,mini::detail::utf8(mini::detail::normalized(file)),false,8);
    mini::forget_root(f.db,42);
    const auto roots=mini::list_roots(f.db);
    require(roots.size()==1 && roots[0].id==7 && roots[0].bytes==11,"forget affected wrong root");
    require(fs::exists(file) && fs::file_size(file)==8,"forget touched disk files");
    require(mini::check_index(f.db).ok,"forget left inconsistent index");
    for (const std::int64_t id : {std::int64_t{0},std::int64_t{-1},std::int64_t{42},std::int64_t{900}})
        rejects([&]{ mini::forget_root(f.db,id); });
    require(mini::index_stats(f.db).roots==1,"invalid forget modified index");
    mini::forget_root(f.db,7);
    require(mini::index_stats(f.db).roots==0 && mini::check_index(f.db).ok,"forget last root failed");
}
/// 在节点删除及根删除阶段分别注入 SQL 故障，验证全部修改都会事务回滚。
void forget_rolls_back() {
    Fixture f; f.two_roots();
    f.sql("CREATE TRIGGER fail_node BEFORE DELETE ON nodes WHEN OLD.id=902 BEGIN SELECT RAISE(ABORT,'node failure'); END;");
    rejects([&]{ mini::forget_root(f.db,42); });
    require(mini::index_stats(f.db).roots==2 && mini::index_stats(f.db).bytes==28,"node failure did not roll back");
    f.sql("DROP TRIGGER fail_node; CREATE TRIGGER fail_root BEFORE DELETE ON roots BEGIN SELECT RAISE(ABORT,'root failure'); END;");
    rejects([&]{ mini::forget_root(f.db,42); });
    require(mini::index_stats(f.db).files==2 && mini::list_roots(f.db)[0].node_id==900,"root failure lost nodes");
    require(mini::check_index(f.db).ok,"rollback left inconsistent tree");
}
/// 其他应用或不兼容版本数据库应被所有管理接口拒绝，且其记录不被修改。
void reject_foreign_index() {
    Fixture f; f.two_roots();
    for (const char* sql : {"PRAGMA application_id=42","PRAGMA application_id=1296386392; PRAGMA user_version=2"}) {
        f.sql(sql);
        rejects([&]{ mini::list_roots(f.db); });
        rejects([&]{ mini::index_stats(f.db); });
        rejects([&]{ mini::check_index(f.db); });
        rejects([&]{ mini::forget_root(f.db,42); });
    }
    f.sql("PRAGMA user_version=1");
    require(mini::index_stats(f.db).files==2,"format rejection changed records");
}
/// 文件总大小超出 int64 时必须显式报错，不能输出溢出或浮点截断结果。
void size_overflow() {
    Fixture f; f.root(1,"root"); f.node(10,1,0,"root",true);
    f.node(11,1,10,"root/huge",false,std::numeric_limits<std::int64_t>::max());
    f.node(12,1,10,"root/extra",false,1);
    rejects([&]{ mini::index_stats(f.db); });
    rejects([&]{ mini::list_roots(f.db); });
}
/// 健康检查应发现根缺失、路径不符、非目录根、多根节点以及外键断链。
void broken_roots() {
    for (const char* sql : {
        "DELETE FROM nodes WHERE root_id=42",
        "UPDATE nodes SET full_path='wrong' WHERE id=900",
        "UPDATE nodes SET is_directory=0 WHERE id=900",
        "UPDATE nodes SET parent_id=NULL WHERE id=902",
        "UPDATE nodes SET root_id=999 WHERE id=902",
        "UPDATE nodes SET parent_id=999 WHERE id=902",
        "PRAGMA ignore_check_constraints=ON; UPDATE nodes SET size=-1 WHERE id=902"
    }) {
        Fixture f; f.two_roots(); f.sql(sql);
        const auto health=mini::check_index(f.db);
        if (health.ok || health.issues.empty()) throw std::runtime_error(std::string("broken root or foreign key not detected: ")+sql);
    }
}
/// 健康检查应在有限时间内发现跨根父链、文件父节点和不连通循环。
void broken_parent_chains() {
    for (const char* sql : {
        "UPDATE nodes SET parent_id=101 WHERE id=902",
        "UPDATE nodes SET parent_id=902 WHERE id=901",
        "UPDATE nodes SET parent_id=902 WHERE id=901; UPDATE nodes SET is_directory=1 WHERE id=902"
    }) {
        Fixture f; f.two_roots(); f.sql(sql);
        const auto health=mini::check_index(f.db);
        require(!health.ok && !health.issues.empty(),"broken parent chain not detected");
    }
}

/// 顺序运行全部真实数据库夹具；单个失败仍继续其他测试以报告完整结果。
int main() {
    int failures=0;
    const std::pair<const char*,void(*)()> tests[] = {
        {"empty_catalog",empty_catalog},{"forget_missing_database",forget_missing_database},
        {"multi_root_stats",multi_root_stats},
        {"forget_preserves_files",forget_preserves_files},{"forget_rolls_back",forget_rolls_back},
        {"reject_foreign_index",reject_foreign_index},{"size_overflow",size_overflow},
        {"broken_roots",broken_roots},{"broken_parent_chains",broken_parent_chains}
    };
    for (const auto& [name,test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    }
    return failures ? 1 : 0;
}
