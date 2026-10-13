#include "mini/index.hpp"
#include "../src/sqlite.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <random>

namespace fs = std::filesystem;
/// 条件不满足时报告独立于被测实现的行为错误。
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
/// 验证操作抛出指定异常类型，避免取消被普通 SQLite 错误替代。
template<class E = std::exception, class F> void rejects(F action) {
    bool rejected = false;
    try { action(); } catch (const E&) { rejected = true; }
    require(rejected, "expected operation to reject");
}
struct Fixture {
    fs::path base = fs::temp_directory_path() / ("mini-maintenance-" + std::to_string(std::random_device{}()));
    fs::path root = base / "root";
    fs::path db = base / "index.db";
    /// 只在临时目录内建立文件夹具，数据库位于扫描范围外。
    Fixture() { fs::create_directories(root); }
    /// 清理本夹具拥有的临时文件，不让清理异常掩盖测试结果。
    ~Fixture() { std::error_code error; fs::remove_all(base, error); }
    /// 在扫描根内写入真实文件并自动创建父目录。
    void file(const fs::path& name, const std::string& bytes = "abc") {
        fs::create_directories((root / name).parent_path());
        std::ofstream stream(root / name, std::ios::binary); stream << bytes;
        require(static_cast<bool>(stream), "fixture file write failed");
    }
    /// 打开真实 SQLite 连接，供故障触发器及断言使用。
    mini::detail::Database open(bool writable = false) {
        const auto value = db.u8string();
        return mini::detail::Database(std::string(reinterpret_cast<const char*>(value.data()), value.size()), writable);
    }
};
/// 读取数据库整数值，验证根身份和数据库关系，而不依赖生产辅助方法。
std::int64_t number(Fixture& fixture, const char* sql) {
    auto db = fixture.open(); mini::detail::Statement statement(db, sql);
    require(statement.row(), "missing fixture SQL result"); return statement.number(0);
}
/// 按保存路径取得全部节点身份，作为失败回滚的可观察快照。
std::map<std::string, std::int64_t> identities(Fixture& fixture) {
    std::map<std::string, std::int64_t> result;
    for (const auto& node : mini::search(fixture.db, {"", "", 10000})) result.emplace(node.path, node.id);
    return result;
}
/// 生成保留节点身份的扫描选项，默认关闭节流以准确观察阶段。
mini::ScanOptions incremental() {
    mini::ScanOptions options; options.incremental = true; options.progress_interval_ms = 0; return options;
}
/// 无变化重扫必须保留所有身份且不触发节点 UPDATE 或 DELETE。
void stable_ids() {
    Fixture fixture; fixture.file("docs/a.txt"); fixture.file("b.txt");
    const auto initial = mini::scan(fixture.root, fixture.db);
    require(initial.inserted == 4 && initial.removed == 0, "initial delta counts wrong");
    const auto before = identities(fixture);
    { auto db = fixture.open(true); db.exec("CREATE TRIGGER reject_update BEFORE UPDATE ON nodes BEGIN SELECT RAISE(ABORT,'unchanged update'); END;"); }
    const auto result = mini::scan(fixture.root, fixture.db, incremental());
    require(result.unchanged == 4 && result.inserted == 0 && result.updated == 0 && result.removed == 0, "unchanged delta counts wrong");
    require(identities(fixture) == before, "unchanged scan replaced identities");
    const auto rebuilt = mini::scan(fixture.root, fixture.db);
    require(rebuilt.inserted == 4 && rebuilt.removed == 4 && rebuilt.updated == 0 && rebuilt.unchanged == 0, "default scan must still fully rebuild");
}
/// 新增、修改和整个子树删除必须只改变目标行，并维持未变化文件的身份。
void deltas_and_roots() {
    Fixture fixture; fixture.file("keep.txt"); fixture.file("change.txt"); fixture.file("old/sub/gone.txt");
    const auto sibling = fixture.base / "sibling"; fs::create_directories(sibling);
    { std::ofstream stream(sibling / "other.txt"); stream << "safe"; }
    mini::scan(fixture.root, fixture.db); mini::scan(sibling, fixture.db);
    const auto keep = mini::search(fixture.db, {"keep.txt"})[0].id;
    const auto other = mini::search(fixture.db, {"other.txt"})[0].id;
    fixture.file("change.txt", "longer bytes"); fixture.file("new.txt"); fs::remove_all(fixture.root / "old");
    const auto result = mini::scan(fixture.root, fixture.db, incremental());
    require(result.inserted == 1 && result.removed == 3 && result.updated >= 1, "insert/update/subtree removal counts wrong");
    require(mini::search(fixture.db, {"change.txt"})[0].size == 12, "changed size missing");
    require(mini::search(fixture.db, {"keep.txt"})[0].id == keep && mini::search(fixture.db, {"other.txt"})[0].id == other, "scan changed unrelated identities");
    require(number(fixture, "SELECT count(*) FROM pragma_foreign_key_check") == 0, "incremental scan broke foreign keys");
}
/// 同一路径文件和目录互换必须保留 ID，并正确连接新增子项及删除旧子项。
void type_replacement() {
    Fixture fixture; fixture.file("switch/child.txt"); mini::scan(fixture.root, fixture.db);
    const auto original = mini::search(fixture.db, {"switch"})[0].id;
    fs::remove_all(fixture.root / "switch"); fixture.file("switch", "leaf");
    const auto to_file = mini::scan(fixture.root, fixture.db, incremental());
    const auto leaf = mini::search(fixture.db, {"switch"})[0];
    require(leaf.id == original && !leaf.is_directory && leaf.size == 4 && to_file.removed == 1, "directory-to-file calibration failed");
    fs::remove(fixture.root / "switch"); fixture.file("switch/new.txt");
    mini::scan(fixture.root, fixture.db, incremental());
    const auto directory = mini::search(fixture.db, {"switch"})[0];
    require(directory.id == original && directory.is_directory && mini::search(fixture.db, {"new.txt"})[0].parent_id == original, "file-to-directory calibration failed");
}
/// 取消与抛错回调必须在各阶段回滚节点和根时间戳。
void callbacks_rollback() {
    Fixture fixture; fixture.file("old.txt"); mini::scan(fixture.root, fixture.db);
    { auto db = fixture.open(true); db.exec("UPDATE roots SET scanned_at=123"); }
    const auto before = identities(fixture); fixture.file("new.txt");
    for (const auto phase : {mini::ScanPhase::preparing, mini::ScanPhase::scanning, mini::ScanPhase::committing}) {
        auto options = incremental(); bool cancel = false;
        // 由进度触发取消，验证提交前的最后一次检查同样有效。
        options.progress = [&](const mini::ScanProgress& progress) { if (progress.phase == phase) cancel = true; };
        // 外部取消状态由扫描轮询，模拟用户取消操作。
        options.cancelled = [&] { return cancel; };
        // 此调用必须保留专门的取消异常类型并回滚所有节点写入。
        rejects<mini::ScanCancelled>([&] { mini::scan(fixture.root, fixture.db, options); });
        require(identities(fixture) == before, "cancel published partial rows");
        require(number(fixture, "SELECT scanned_at FROM roots") == 123, "cancel published root timestamp");
        options.cancelled = {};
        // 进度回调的应用异常必须原样传播，不能在已提交之后调用。
        options.progress = [=](const mini::ScanProgress& progress) { if (progress.phase == phase) throw std::logic_error("progress failure"); };
        // 此调用验证回调异常同样走回滚路径。
        rejects<std::logic_error>([&] { mini::scan(fixture.root, fixture.db, options); });
        require(identities(fixture) == before, "progress failure published rows");
        require(number(fixture, "SELECT scanned_at FROM roots") == 123, "callback failure published root timestamp");
    }
    auto options = incremental(); std::vector<mini::ScanProgress> events;
    // 保存真实进度以检查阶段顺序和最后的文件/目录计数。
    options.progress = [&](const mini::ScanProgress& progress) { events.push_back(progress); };
    const auto result = mini::scan(fixture.root, fixture.db, options);
    require(events.size() >= 3 && events.front().phase == mini::ScanPhase::preparing && events.back().phase == mini::ScanPhase::committing, "progress phases missing");
    require(events.back().files == result.files && events.back().directories == result.directories, "progress final counts wrong");
}
/// 实际写入一条文件后取消或抛错，仍必须恢复提交前快照；长节流只保留三个阶段边界。
void progress_and_partial_write() {
    Fixture fixture; fixture.file("old.txt"); mini::scan(fixture.root, fixture.db);
    const auto before = identities(fixture); fixture.file("new.txt");
    for (bool callback_throws : {false, true}) {
        auto options = incremental(); bool cancel = false;
        // 有实际文件写入后才触发故障，防止测试仅覆盖阶段入口。
        options.progress = [&](const mini::ScanProgress& progress) {
            if (progress.phase == mini::ScanPhase::scanning && progress.files > 0) {
                if (callback_throws) throw std::logic_error("partial scan failure");
                cancel = true;
            }
        };
        // 将进度回调设置的状态转换为扫描取消请求。
        options.cancelled = [&] { return cancel; };
        if (callback_throws) {
            // 已写节点之后的进度异常必须保留原始异常类别。
            rejects<std::logic_error>([&] { mini::scan(fixture.root, fixture.db, options); });
        } else {
            // 已写节点之后的取消必须回滚，不能发布部分文件。
            rejects<mini::ScanCancelled>([&] { mini::scan(fixture.root, fixture.db, options); });
        }
        require(identities(fixture) == before, "partial scan published rows");
    }
    auto options = incremental(); options.progress_interval_ms = 2147483647; std::vector<mini::ScanPhase> phases;
    // 超长节流间隔应抑制逐条扫描事件，但不得隐藏各阶段入口。
    options.progress = [&](const mini::ScanProgress& progress) { phases.push_back(progress.phase); };
    mini::scan(fixture.root, fixture.db, options);
    require(phases == std::vector<mini::ScanPhase>{mini::ScanPhase::preparing, mini::ScanPhase::scanning, mini::ScanPhase::committing}, "throttling hid phases or emitted redundant progress");
}
/// 大型 DELETE 必须被 SQLite 进度处理器取消；处理器中的异常不得穿过 C 边界。
void sql_cancellation() {
    Fixture fixture; fixture.file("keep.txt"); fixture.file("stale.txt"); mini::scan(fixture.root, fixture.db); fs::remove(fixture.root / "stale.txt");
    { auto db = fixture.open(true); db.exec("CREATE TRIGGER expensive_delete BEFORE DELETE ON nodes BEGIN SELECT sum(x) FROM (WITH RECURSIVE r(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM r WHERE x<1000000) SELECT x FROM r); END;"); }
    const auto before = identities(fixture);
    for (bool use_incremental : {false, true}) for (bool callback_throws : {false, true}) {
        mini::ScanOptions options; options.incremental = use_incremental; int polls = 0;
        // 前一百次正常检查不会在准备阶段退出，昂贵 DELETE 内的轮询最终触发取消或应用错误。
        options.cancelled = [&] { if (++polls > 100) { if (callback_throws) throw std::logic_error("cancel probe failed"); return true; } return false; };
        if (callback_throws) {
            // C 回调捕获异常后，扫描需在 C++ 边界重新抛出原始应用异常。
            rejects<std::logic_error>([&] { mini::scan(fixture.root, fixture.db, options); });
        } else {
            // 大型 SQL 的 SQLITE_INTERRUPT 必须转换为专用取消异常。
            rejects<mini::ScanCancelled>([&] { mini::scan(fixture.root, fixture.db, options); });
        }
        require(polls > 100 && identities(fixture) == before, "SQL cancellation failed to roll back");
    }
}
/// 更正所有真实元数据字段，同时检查 UPDATE 失败会恢复旧值和原节点身份。
void metadata_update() {
    Fixture fixture; fixture.file("folder/keep.txt", "abc"); mini::scan(fixture.root, fixture.db);
    const auto original = mini::search(fixture.db, {"keep.txt"})[0];
    { auto db = fixture.open(true); db.exec("UPDATE nodes SET parent_id=NULL,name='wrong',extension='.bad',size=999,modified_at=0 WHERE is_directory=0"); }
    const auto result = mini::scan(fixture.root, fixture.db, incremental());
    const auto corrected = mini::search(fixture.db, {"keep.txt", "txt"})[0];
    require(result.updated == 1 && corrected.id == original.id && corrected.parent_id == original.parent_id && corrected.size == 3 && corrected.modified_at == original.modified_at, "metadata calibration did not repair changed fields");
    fixture.file("folder/keep.txt", "longer");
    { auto db = fixture.open(true); db.exec("CREATE TRIGGER fail_update BEFORE UPDATE ON nodes BEGIN SELECT RAISE(ABORT,'update failure'); END;"); }
    // 变化文件触发 UPDATE 失败，已保存的元数据应保持不变。
    rejects([&] { mini::scan(fixture.root, fixture.db, incremental()); });
    const auto preserved = mini::search(fixture.db, {"keep.txt"})[0];
    require(preserved.id == original.id && preserved.parent_id == original.parent_id && preserved.size == 3, "failed update changed persisted metadata");
}
/// 扫描写入失败时必须恢复旧 ID 和父关系，默认重建与增量模式都适用。
void write_failure() {
    Fixture fixture; fixture.file("folder/old.txt"); mini::scan(fixture.root, fixture.db);
    const auto before = identities(fixture); fixture.file("new.txt");
    { auto db = fixture.open(true); db.exec("CREATE TRIGGER fail_insert BEFORE INSERT ON nodes BEGIN SELECT RAISE(ABORT,'disk failure'); END;"); }
    for (bool use_incremental : {false, true}) {
        mini::ScanOptions options; options.incremental = use_incremental;
        // INSERT 故障不能发布此前发生的 DELETE 或 UPDATE。
        rejects([&] { mini::scan(fixture.root, fixture.db, options); });
        require(identities(fixture) == before, "write failure lost old identities");
    }
}
/// expected_root_id 必须在写事务内约束已注册路径，非法配置不得创建数据库或重新添加根。
void root_and_options() {
    Fixture fixture; fixture.file("one.txt"); mini::scan(fixture.root, fixture.db);
    const auto root_id = number(fixture, "SELECT id FROM roots");
    auto options = incremental(); options.expected_root_id = root_id; mini::scan(fixture.root, fixture.db, options);
    options.expected_root_id = root_id + 100;
    // 根身份失配必须拒绝校准。
    rejects<std::invalid_argument>([&] { mini::scan(fixture.root, fixture.db, options); });
    const auto other = fixture.base / "other"; fs::create_directories(other); options.expected_root_id = root_id;
    // 已知根 ID 搭配不同路径不得注册新根。
    rejects<std::invalid_argument>([&] { mini::scan(other, fixture.db, options); });
    { auto db = fixture.open(true); db.exec("DELETE FROM nodes; DELETE FROM roots;"); }
    // 被 forget 的根不得被监听校准重新加入。
    rejects<std::invalid_argument>([&] { mini::scan(fixture.root, fixture.db, options); });
    require(number(fixture, "SELECT count(*) FROM roots") == 0, "expected root re-added forgotten root");
    for (int kind = 0; kind < 3; ++kind) {
        options = {}; if (kind == 0) options.progress_interval_ms = -1; else if (kind == 1) options.expected_root_id = -1; else options.db_cache_mib = 0;
        const auto missing = fixture.base / ("invalid-" + std::to_string(kind) + ".db");
        // 非法参数在打开数据库前失败，避免创建无用文件。
        rejects<std::invalid_argument>([&] { mini::scan(fixture.root, missing, options); });
        require(!fs::exists(missing), "invalid options created a database");
    }
}
/// 增量模式仍应拒绝扫描数据库硬链接，并回滚已有节点。
void hardlink_protection() {
    Fixture fixture; fixture.file("one.txt"); mini::scan(fixture.root, fixture.db);
    const auto before = identities(fixture); fs::create_hard_link(fixture.db, fixture.root / "alias.db");
    // 数据库别名属于扫描范围内的同一文件，不能成为普通节点。
    rejects<std::invalid_argument>([&] { mini::scan(fixture.root, fixture.db, incremental()); });
    require(identities(fixture) == before, "hardlink failure modified index");
}
#ifdef _WIN32
/// 从真实数据库读取已注册根路径，供大小写重命名和回滚断言使用。
std::string registered_root_path(Fixture& fixture) {
    auto db = fixture.open(); mini::detail::Statement statement(db, "SELECT path FROM roots");
    require(statement.row(), "registered root missing"); return statement.text(0);
}
/// 经中间目录名完成 Windows 大小写重命名，避免文件系统将直接改名视为同名操作。
void rename_root_case(Fixture& fixture) {
    const auto intermediate = fixture.base / "rename-intermediate";
    const auto renamed = fixture.base / "ROOT";
    fs::rename(fixture.root, intermediate); fs::rename(intermediate, renamed); fixture.root = renamed;
}
/// 根目录仅改变大小写时，两种扫描模式均须同步注册路径与根节点路径并保留根身份。
void root_case_rename(bool use_incremental) {
    Fixture fixture; fixture.file("folder/keep.txt"); mini::scan(fixture.root, fixture.db);
    const auto root_id = number(fixture, "SELECT id FROM roots");
    const auto original_path = registered_root_path(fixture); rename_root_case(fixture);
    mini::ScanOptions options; options.incremental = use_incremental; options.expected_root_id = root_id;
    mini::scan(fixture.root, fixture.db, options);
    require(number(fixture, "SELECT count(*) FROM roots") == 1 && number(fixture, "SELECT id FROM roots") == root_id, "case rename replaced registered root ID");
    require(number(fixture, "SELECT count(*) FROM nodes n JOIN roots r ON n.root_id=r.id WHERE n.parent_id IS NULL AND n.full_path<>r.path") == 0,
        use_incremental ? "incremental case rename left stale registered root path" : "default case rename left stale registered root path");
    const auto renamed_path = registered_root_path(fixture);
    require(renamed_path != original_path && renamed_path.ends_with("\\ROOT"), "registered root path did not preserve current filesystem spelling");
}
/// 默认重建需要保持已注册根与新根节点的当前路径拼写一致。
void root_case_rename_default() { root_case_rename(false); }
/// 增量校准需要保持已注册根与新根节点的当前路径拼写一致。
void root_case_rename_incremental() { root_case_rename(true); }
/// 同步根路径之后发生取消、进度异常或 SQL 故障时，两种扫描模式都须恢复旧路径与节点。
void root_case_rename_rollback() {
    for (bool use_incremental : {false, true}) {
        Fixture fixture; fixture.file("folder/keep.txt"); mini::scan(fixture.root, fixture.db);
        const auto root_id = number(fixture, "SELECT id FROM roots");
        const auto original_path = registered_root_path(fixture); const auto before = identities(fixture);
        rename_root_case(fixture);
        for (bool callback_throws : {false, true}) {
            mini::ScanOptions options; options.incremental = use_incremental; options.expected_root_id = root_id; bool cancel = false;
            // 在提交阶段才故障，使根路径同步和节点写入均已发生但尚未提交。
            options.progress = [&](const mini::ScanProgress& progress) {
                if (progress.phase == mini::ScanPhase::committing) {
                    if (callback_throws) throw std::logic_error("renamed root callback failure");
                    cancel = true;
                }
            };
            // 接收提交前进度设置的取消状态。
            options.cancelled = [&] { return cancel; };
            if (callback_throws) {
                // 应用回调故障必须恢复旧注册路径。
                rejects<std::logic_error>([&] { mini::scan(fixture.root, fixture.db, options); });
            } else {
                // 用户取消必须恢复旧注册路径。
                rejects<mini::ScanCancelled>([&] { mini::scan(fixture.root, fixture.db, options); });
            }
            require(registered_root_path(fixture) == original_path && identities(fixture) == before, "cancel or callback published renamed root path");
        }
        { auto db = fixture.open(true); db.exec("CREATE TRIGGER renamed_insert_failure BEFORE INSERT ON nodes BEGIN SELECT RAISE(ABORT,'renamed root SQL failure'); END;"); }
        mini::ScanOptions options; options.incremental = use_incremental; options.expected_root_id = root_id;
        // 节点插入失败发生在根路径同步之后，整个写事务必须一起回滚。
        rejects([&] { mini::scan(fixture.root, fixture.db, options); });
        require(registered_root_path(fixture) == original_path && identities(fixture) == before, "SQL failure published renamed root path");
    }
}
#endif
/// 无参数执行维护模块全部真实夹具测试，报告每个用例的独立结果。
int main() {
    const std::pair<const char*, void(*)()> tests[] = {
        {"stable_ids", stable_ids}, {"deltas_and_roots", deltas_and_roots}, {"type_replacement", type_replacement},
        {"callbacks_rollback", callbacks_rollback}, {"sql_cancellation", sql_cancellation},
        {"progress_and_partial_write", progress_and_partial_write}, {"metadata_update", metadata_update},
        {"write_failure", write_failure}, {"root_and_options", root_and_options}, {"hardlink_protection", hardlink_protection}
#ifdef _WIN32
        , {"root_case_rename_default", root_case_rename_default}, {"root_case_rename_incremental", root_case_rename_incremental}
        , {"root_case_rename_rollback", root_case_rename_rollback}
#endif
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    return failures == 0 ? 0 : 1;
}
