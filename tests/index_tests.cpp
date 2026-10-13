#include "mini/index.hpp"
#include "../src/sqlite.hpp"
#include <sqlite3.h>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../src/windows_fs.hpp"
#endif

namespace fs = std::filesystem;
/// 测试断言助手：条件不成立时抛出带说明的异常，由测试入口报告失败。
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
/// 执行预期会失败的测试回调，确认它抛出标准异常；正常返回则视为断言失败。
template<class F> void rejects(F action) {
    bool thrown = false;
    try { action(); } catch (const std::exception&) { thrown = true; }
    require(thrown, "operation should reject invalid input");
}
struct Fixture {
    fs::path base = fs::temp_directory_path() / ("mini-test-" + std::to_string(std::random_device{}()));
    fs::path root = base / "root";
    fs::path db = base / "index.db";
    /// 创建随机命名的临时测试根目录，并预留位于扫描范围外的数据库路径。
    Fixture() { fs::create_directories(root); }
    /// 尽力清理本夹具的临时目录树，使用 error_code 避免析构期间抛出异常。
    ~Fixture() { std::error_code ec; fs::remove_all(base, ec); }
    /// 在测试根下创建指定文件并写入字节内容，自动建立父目录，写入失败则抛错。
    void file(const fs::path& name, const std::string& data = "abc") {
        fs::create_directories((root / name).parent_path());
        std::ofstream out(root / name, std::ios::binary);
        out << data;
        if (!out) throw std::runtime_error("fixture write failed");
    }
};
/// 临时打开夹具数据库执行 SQL，用于布置外部格式、版本标记或故障触发器，并检查执行结果。
void sql(const fs::path& file, const char* statement) {
    sqlite3* db = nullptr;
    const auto utf8 = file.u8string();
    require(sqlite3_open(reinterpret_cast<const char*>(utf8.c_str()), &db) == SQLITE_OK, "fixture db open failed");
    const int result = sqlite3_exec(db, statement, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    require(result == SQLITE_OK, "fixture SQL failed");
}

/// 验证扫描数量、空目录、父子节点关系、文件大小和修改时间都正确写入索引。
void scan_tree() {
    Fixture f;
    f.file("docs/report.pdf", "hello");
    fs::create_directories(f.root / "empty");
    const auto result = mini::scan(f.root, f.db);
    require(result.files == 1 && result.directories == 3, "wrong scan totals");
    const auto docs = mini::search(f.db, {"docs"});
    const auto report = mini::search(f.db, {"report"});
    require(docs.size() == 1 && docs[0].is_directory, "directory not indexed");
    require(report.size() == 1 && report[0].parent_id == docs[0].id, "broken parent link");
    require(report[0].size == 5 && !report[0].is_directory, "file metadata incorrect");
    require(report[0].modified_at > 0, "mtime missing");
}
/// 删除实际目录后查询已有数据库，验证搜索读取持久化索引而非重新遍历文件系统。
void persistence() {
    Fixture f; f.file("offline.txt");
    mini::scan(f.root, f.db);
    fs::remove_all(f.root);
    require(mini::search(f.db, {"offline"}).size() == 1, "search should use persisted index");
}
/// 通过删除旧文件、增加新文件及重复重扫，验证旧记录清理、元数据更新和无重复节点。
void rescan() {
    Fixture f; f.file("old.txt"); mini::scan(f.root, f.db);
    fs::remove(f.root / "old.txt"); f.file("new.txt", "123456");
    mini::scan(f.root, f.db); mini::scan(f.root, f.db);
    require(mini::search(f.db, {"old"}).empty(), "stale record survives rescan");
    const auto found = mini::search(f.db, {"new"});
    require(found.size() == 1 && found[0].size == 6, "rescan duplicated or lost metadata");
    require(mini::search(f.db, {""}).size() == 2, "unexpected duplicate nodes");
}
/// 验证百分号、下划线和引号按字面文本查询，并确认用户输入不会改变 SQL 查询结构。
void literal_search() {
    Fixture f; f.file("100%_report.txt"); f.file("100xxreport.txt"); f.file("O'Brien.txt");
    mini::scan(f.root, f.db);
    require(mini::search(f.db, {"%_"}).size() == 1, "LIKE wildcards not escaped");
    require(mini::search(f.db, {"O'Brien"}).size() == 1, "quote handling incorrect");
    require(mini::search(f.db, {"' OR 1=1 --"}).empty(), "SQL injection");
}
/// 验证 ASCII 大小写、扩展名、文件/目录区分以及查询结果数量的合法范围。
void filters() {
    Fixture f; f.file("REPORT.PDF"); f.file("report.txt"); f.file("another.pdf");
    fs::create_directory(f.root / "folder.pdf"); mini::scan(f.root, f.db);
    auto found = mini::search(f.db, {"report", ".PDF", 100});
    require(found.size() == 1 && found[0].name == "REPORT.PDF", "case or extension filtering failed");
    require(mini::search(f.db, {"", "pdf"}).size() == 2, "extension filter includes directory");
    require(mini::search(f.db, {"", "", 1}).size() == 1, "limit not enforced");
    // 此回调请求零条结果，用来验证查询数量的下界检查。
    rejects([&] { mini::search(f.db, {"", "", 0}); });
    // 此回调请求超过上限的结果数量，用来验证查询数量的上界检查。
    rejects([&] { mini::search(f.db, {"", "", 10001}); });
}
/// 用中文及带空格的目录和数据库名验证原生路径、UTF-8 存储与查询的往返一致性。
void unicode() {
    Fixture f; f.file(fs::path(u8"资料 空间/年度报告.txt"));
    f.db = f.base / fs::path(u8"中文索引.db"); mini::scan(f.root, f.db);
    const auto found = mini::search(f.db, {"年度报告"});
    require(found.size() == 1 && found[0].name == "年度报告.txt", "UTF-8 round trip failed");
}
/// 验证祖先/后代根不能重复索引，而互不重叠的兄弟根可共存且不会清除已有数据。
void overlap() {
    Fixture f; f.file("child/a.txt"); mini::scan(f.root, f.db);
    // 此回调索引已有根的后代目录，预期被根范围重叠检查拒绝。
    rejects([&] { mini::scan(f.root / "child", f.db); });
    const auto sibling = f.base / "root-sibling";
    fs::create_directory(sibling); mini::scan(sibling, f.db);
    require(mini::search(f.db, {"a.txt"}).size() == 1, "second root erased first");
    // Ancestor rejection with database outside both roots.
    Fixture g; g.file("child/a.txt"); mini::scan(g.root / "child", g.db);
    // 此回调索引已有根的祖先目录，验证另一方向的重叠同样被拒绝。
    rejects([&] { mini::scan(g.root, g.db); });
}
/// 验证不存在的根、文件根和不存在的查询数据库被拒绝，且旧索引不会受损或被悄悄创建。
void invalid_root() {
    Fixture f; f.file("a.txt"); mini::scan(f.root, f.db);
    // 此回调提交不存在的目录，验证扫描根必须可访问。
    rejects([&] { mini::scan(f.root / "missing", f.db); });
    // 此回调把普通文件当作扫描根，验证扫描只接受目录。
    rejects([&] { mini::scan(f.root / "a.txt", f.db); });
    require(mini::search(f.db, {"a.txt"}).size() == 1, "invalid scan destroyed index");
    // 此回调查询不存在的数据库，验证只读查询不会隐式创建索引。
    rejects([&] { mini::search(f.base / "missing.db", {"a"}); });
    require(!fs::exists(f.base / "missing.db"), "search silently created db");
}
/// 验证数据库位于扫描范围内时立即拒绝，且不会先创建不安全的索引文件。
void database_inside() {
    Fixture f;
    // 此回调把数据库放入扫描根内，验证索引自身保护在创建文件前生效。
    rejects([&] { mini::scan(f.root, f.root / "index.db"); });
    require(!fs::exists(f.root / "index.db"), "unsafe database created");
}
/// 构建多层目录并检查完整节点数量，验证迭代 DFS 能覆盖深层子树。
void deep_tree() {
    Fixture f; fs::path p;
    for (int i = 0; i < 35; ++i) p /= "d";
    f.file(p / "leaf.txt");
    const auto result = mini::scan(f.root, f.db);
    require(result.directories == 36 && result.files == 1, "deep traversal failed");
}
/// 在写入阶段注入 SQLite 触发器故障，验证重扫事务回滚并保留上次成功的节点集合。
void rollback() {
    Fixture f; f.file("old.txt"); mini::scan(f.root, f.db);
    // A real SQLite failure after the transaction has started must restore old nodes.
    sql(f.db, "CREATE TRIGGER fail_insert BEFORE INSERT ON nodes BEGIN SELECT RAISE(ABORT, 'test disk failure'); END;");
    fs::remove(f.root / "old.txt"); f.file("new.txt");
    // 此回调触发预先布置的写入故障，让下面的断言检查事务是否完整回滚。
    rejects([&] { mini::scan(f.root, f.db); });
    require(mini::search(f.db, {"old.txt"}).size() == 1, "failed scan removed old snapshot");
    require(mini::search(f.db, {"new.txt"}).empty(), "failed scan published partial result");
}
/// 验证扫描和查询拒绝外部 SQLite 数据库，且拒绝后其原表仍可正常使用。
void foreign_database() {
    Fixture f; sql(f.db, "CREATE TABLE unrelated(value TEXT);");
    // 此扫描回调尝试使用外部数据库，验证格式校验会保护原有表。
    rejects([&] { mini::scan(f.root, f.db); });
    // 此查询回调尝试读取外部数据库，验证只读路径也执行格式校验。
    rejects([&] { mini::search(f.db, {""}); });
    sql(f.db, "INSERT INTO unrelated VALUES ('still usable');");
}
/// 将索引版本改为不兼容值并检查拒绝行为，恢复版本后确认旧数据未被改写。
void schema_version() {
    Fixture f; f.file("report.txt"); mini::scan(f.root, f.db);
    sql(f.db, "PRAGMA user_version=99;");
    // 此回调查询不兼容版本的索引，预期被版本检查拒绝。
    rejects([&] { mini::search(f.db, {""}); });
    // 此回调重扫不兼容版本的索引，验证写入路径也不会覆盖旧数据。
    rejects([&] { mini::scan(f.root, f.db); });
    sql(f.db, "PRAGMA user_version=1;");
    require(mini::search(f.db, {"report.txt"}).size() == 1, "unsupported schema was modified");
}
/// 在扫描范围内建立索引数据库的硬链接，验证身份检查会拒绝它并保留旧索引。
void database_hardlink() {
    Fixture f; f.file("old.txt"); mini::scan(f.root, f.db);
    fs::create_hard_link(f.db, f.root / "alias.db");
    // 此回调扫描含有数据库硬链接的目录，预期身份检查阻止提交。
    rejects([&] { mini::scan(f.root, f.db); });
    require(mini::search(f.db, {"alias.db"}).empty(), "database hardlink was indexed");
    require(mini::search(f.db, {"old.txt"}).size() == 1, "hardlink rejection lost old index");
}
/// 验证默认/自定义缓存、prepare 子阶段与提交后的统计，并检查非法缓存参数不触碰索引。
void scan_cache_profile() {
    Fixture f; f.file("child/one.txt");
    const auto initial = mini::scan(f.root, f.db);
    require(initial.database_stats.cache_mib == 64 && initial.timings.clear_ms == 0, "new index must use default cache and skip cleanup");
    fs::remove(f.root / "child/one.txt"); f.file("child/two.txt", "12345");
    const auto result = mini::scan(f.root, f.db, {2});
    require(result.database_stats.cache_mib == 2 && result.timings.clear_ms > 0, "rescan cache or cleanup timing missing");
    const auto& t = result.timings;
    require(t.prepare_ms + 0.01 >= t.open_ms+t.transaction_ms+t.schema_ms+t.roots_ms+t.clear_ms+t.statements_ms,
        "prepare timing must contain its subphases");
    require(result.database_stats.cache_hits > 0 && result.database_stats.cache_writes > 0, "cache statistics must include committed scan");
    const auto found = mini::search(f.db, {"two.txt"});
    require(found.size() == 1 && found[0].size == 5 && mini::search(f.db, {"one.txt"}).empty(), "custom cache rescan lost changes");
    // 此循环回调逐个提交非法缓存目标，验证 API 在修改旧索引前拒绝参数。
    for (const int invalid : {0, -1, 1025}) rejects([&] { mini::scan(f.root, f.db, {invalid}); });
    require(mini::search(f.db, {"two.txt"}).size() == 1, "invalid cache modified index");
    const auto missing = f.base / "missing.db";
    // 此回调同时使用新数据库路径与非法缓存值，验证参数失败不会创建数据库。
    rejects([&] { mini::scan(f.root, missing, {0}); });
    require(!fs::exists(missing), "invalid cache created database");
}
/// 用中文数据库名验证缓存目标仅在当前连接生效，同时保持外键、同步和脏页溢出策略。
void database_cache_policy() {
    Fixture f; f.file("one.txt"); f.db = f.base / fs::path(u8"缓存索引.db"); mini::scan(f.root, f.db);
    const auto native_utf8 = f.db.u8string();
    const std::string database_utf8(reinterpret_cast<const char*>(native_utf8.data()), native_utf8.size());
    // 读取 PRAGMA 的首行整数值，供缓存和数据库安全策略断言使用。
    auto scalar = [](mini::detail::Database& db, const char* sql) {
        mini::detail::Statement statement(db, sql); require(statement.row(), "missing pragma value"); return statement.number(0);
    };
    {
        mini::detail::Database db(database_utf8, true, 64);
        require(scalar(db, "PRAGMA cache_size") == -65536, "cache target not applied");
        require(scalar(db, "PRAGMA foreign_keys") == 1 && scalar(db, "PRAGMA trusted_schema") == 0, "cache changed integrity policy");
        require(scalar(db, "PRAGMA synchronous") == 2 && scalar(db, "PRAGMA cache_spill") > 0, "cache changed durability or spill policy");
        mini::detail::Statement mode(db, "PRAGMA journal_mode"); require(mode.row() && mode.text(0) == "delete", "cache changed journal mode");
        // 此回调插入引用不存在根的节点，验证缓存调节没有关闭外键约束。
        rejects([&] { db.exec("INSERT INTO nodes(root_id,parent_id,name,full_path,extension,is_directory,size,modified_at) VALUES(999999,NULL,'bad','bad','',0,0,0)"); });
    }
    mini::detail::Database reopened(database_utf8, false);
    require(scalar(reopened, "PRAGMA cache_size") != -65536, "scan cache leaked into future read connections");
    require(mini::search(f.db, {"one.txt"}).size() == 1, "cache policy damaged persisted index");
}
/// 验证低缓存下重扫只替换目标根；写入失败时同时保留原节点身份、父关系及其他根数据。
void rescan_multiple_roots() {
    Fixture f; f.file("old/leaf.txt");
    const auto sibling = f.base / "sibling"; fs::create_directories(sibling);
    { std::ofstream out(sibling / "keep.txt"); out << "keep"; }
    mini::scan(f.root, f.db); mini::scan(sibling, f.db);
    fs::remove_all(f.root / "old"); f.file("new/leaf.txt", "longer");
    mini::scan(f.root, f.db, {1});
    const auto parents = mini::search(f.db, {"new"}); const auto leaves = mini::search(f.db, {"leaf.txt"});
    require(mini::search(f.db, {"old"}).empty() && mini::search(f.db, {"keep.txt"}).size() == 1, "rescan erased other root or retained stale nodes");
    require(parents.size() == 1 && leaves.size() == 1 && leaves[0].parent_id == parents[0].id, "rescan broke parent relationships");
    sql(f.db, "CREATE TRIGGER fail_insert BEFORE INSERT ON nodes BEGIN SELECT RAISE(ABORT, 'test disk failure'); END;");
    // 此回调在低缓存、多根环境下触发重扫写入故障，供后续断言检查回滚边界。
    f.file("failed.txt"); rejects([&] { mini::scan(f.root, f.db, {1}); });
    require(mini::search(f.db, {"failed.txt"}).empty() && mini::search(f.db, {"keep.txt"}).size() == 1,
        "failed rescan published changes or lost sibling");
    const auto preserved = mini::search(f.db, {"leaf.txt"});
    require(preserved.size() == 1 && preserved[0].id == leaves[0].id && preserved[0].parent_id == leaves[0].parent_id,
        "failed rescan lost original node identities");
}
#ifdef _WIN32
struct SkipTest : std::runtime_error { using std::runtime_error::runtime_error; };
/// 用本地路径与 localhost 管理共享访问同一数据库，验证两种别名方向都能阻止索引自身。
/// 管理共享不可用时抛出 SkipTest，让 CTest 将环境不足标记为跳过。
void windows_unc_alias() {
    Fixture f; f.file("hello.txt");
    const auto native = f.root.wstring();
    const auto unc = fs::path(L"\\\\localhost\\" + native.substr(0, 1) + L"$" + native.substr(2));
    if (GetFileAttributesW(unc.c_str()) == INVALID_FILE_ATTRIBUTES)
        throw SkipTest("localhost administrative share is unavailable");
    const auto local_db = f.root / "index.db";
    const auto unc_db = unc / "index.db";
    // 此回调用本地根和 UNC 数据库别名验证同一文件不能被纳入索引。
    rejects([&] { mini::scan(f.root, unc_db); });
    // 此回调交换两种访问路径，验证 UNC 根配本地数据库时保护同样有效。
    rejects([&] { mini::scan(unc, local_db); });
}
/// 验证卷根包含其后代，并在真正枚举磁盘前拒绝位于该卷内的索引数据库。
void windows_volume_root() {
    Fixture f;
    const auto root = mini::detail::normalized(f.root.root_path());
    const auto db = mini::detail::normalized(f.db);
    require(mini::detail::contains(root, db), "volume root must contain its descendants");
    // Only reached after the containment assertion: never traverse a real volume.
    // 此回调验证数据库位于卷根内时被拒绝；先前断言保证不会开始枚举整个卷。
    rejects([&] { mini::scan(f.root.root_path(), f.db); });
    require(!fs::exists(f.db), "volume scan created an index inside its own root");
}
/// 验证单链接本地库省去逐文件身份检查，多链接库执行检查，链接数变化会阻止提交。
void windows_identity_guard() {
    Fixture f; f.file("one.txt"); f.file("two.txt");
    const auto initial = mini::scan(f.root, f.db);
    require(initial.files == 2 && initial.identity_checks == 0, "single-link database should avoid per-file identity opens");
    fs::create_hard_link(f.db, f.base / "outside-alias.db");
    const auto fallback = mini::scan(f.root, f.db);
    require(fallback.files == 2 && fallback.identity_checks == 2, "multi-link database must compare every file");
    mini::detail::IndexFileGuard guard(f.db);
    require(guard.matches(f.base / "outside-alias.db"), "cached database identity did not match hardlink");
    fs::create_hard_link(f.db, f.base / "second-alias.db");
    // 此回调重新检查链接数，验证初始检查之后增加的链接也会阻止提交。
    rejects([&] { guard.verify_unchanged(); });
}
/// 用独占句柄锁住子目录制造读取失败，验证重扫停止并回滚到旧索引。
void windows_read_failure() {
    Fixture f; f.file("child/old.txt"); mini::scan(f.root, f.db);
    HANDLE handle = CreateFileW((f.root / "child").c_str(), GENERIC_READ, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "could not lock fixture directory");
    // 此回调访问被锁住的子目录，验证读取失败会传播；外层捕获负责及时关闭测试句柄。
    try { rejects([&] { mini::scan(f.root, f.db); }); }
    catch (...) { CloseHandle(handle); throw; }
    CloseHandle(handle);
    require(mini::search(f.db, {"old.txt"}).size() == 1, "directory read failure destroyed old index");
}
/// 验证 Windows 扩展路径输入可扫描和查询，普通路径重扫后不会创建重复的根或节点。
void windows_extended_input() {
    Fixture f; f.file("report.txt");
    const auto root = fs::path(L"\\\\?\\" + f.root.wstring());
    const auto db = fs::path(L"\\\\?\\" + f.db.wstring());
    mini::scan(root, db);
    require(mini::search(db, {"report"}).size() == 1, "extended input path failed");
    mini::scan(f.root, f.db);
    require(mini::search(db, {"report"}).size() == 1, "extended input duplicated root");
}
/// 在空工作目录下扫描超过 260 字符的深层路径，验证节点与文件大小完整且不会静默漏项。
void windows_long_tree() {
    Fixture f;
    // Avoid MinGW remove_all on the long fixture; delete only known owned entries.
    struct LongCleanup {
        std::vector<std::wstring> directories;
        std::wstring leaf;
        /// 用原生 API 删除本夹具记录的长路径文件和目录，避免 MinGW 递归清理长路径的缺陷。
        ~LongCleanup() {
            if (!leaf.empty()) DeleteFileW(leaf.c_str());
            for (auto it = directories.rbegin(); it != directories.rend(); ++it) RemoveDirectoryW(it->c_str());
        }
    } cleanup;
    auto path = L"\\\\?\\" + f.root.wstring();
    for (int i=0; i<4; ++i) {
        path += L"\\segment-" + std::to_wstring(i) + std::wstring(60, L'x');
        require(CreateDirectoryW(path.c_str(), nullptr) != 0, "long fixture mkdir failed");
        cleanup.directories.push_back(path);
    }
    const auto leaf = path + L"\\leaf.txt";
    cleanup.leaf = leaf;
    HANDLE h = CreateFileW(leaf.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(h != INVALID_HANDLE_VALUE, "long fixture write failed");
    DWORD written = 0;
    const bool wrote = WriteFile(h, "hello", 5, &written, nullptr) != 0;
    CloseHandle(h);
    require(wrote && written == 5, "long fixture bytes failed");
    fs::create_directory(f.base / "empty-cwd");
    const auto previous = fs::current_path();
    fs::current_path(f.base / "empty-cwd");
    try {
        const auto result = mini::scan(f.root, f.db);
        require(result.files == 1 && result.directories == 5, "long tree silently truncated");
        const auto found = mini::search(f.db, {"leaf"});
        require(found.size() == 1 && found[0].size == 5, "long leaf metadata missing");
        fs::current_path(previous);
    } catch (...) { fs::current_path(previous); throw; }
}
#endif

/// 根据参数选择一个测试用例并执行，输出 PASS/FAIL；环境不足的 Windows 用例返回跳过码 77。
int main(int argc, char** argv) {
    const std::map<std::string, std::function<void()>> cases = {
        {"scan_tree", scan_tree}, {"persistence", persistence}, {"rescan", rescan},
        {"literal_search", literal_search}, {"filters", filters}, {"unicode", unicode},
        {"overlap", overlap}, {"invalid_root", invalid_root}, {"database_inside", database_inside},
        {"deep_tree", deep_tree}, {"rollback", rollback}, {"foreign_database", foreign_database},
        {"schema_version", schema_version}, {"database_hardlink", database_hardlink},
        {"scan_cache_profile", scan_cache_profile}, {"database_cache_policy", database_cache_policy},
        {"rescan_multiple_roots", rescan_multiple_roots}
#ifdef _WIN32
        , {"windows_extended_input", windows_extended_input}, {"windows_long_tree", windows_long_tree}
        , {"windows_identity_guard", windows_identity_guard}, {"windows_read_failure", windows_read_failure}
        , {"windows_volume_root", windows_volume_root}
        , {"windows_unc_alias", windows_unc_alias}
#endif
    };
    try {
        require(argc == 2, "expected case name"); cases.at(argv[1])();
        std::cout << "PASS " << argv[1] << '\n'; return 0;
    }
#ifdef _WIN32
    catch (const SkipTest& e) { std::cout << "SKIP: " << e.what() << '\n'; return 77; }
#endif
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
