#include "mini/index.hpp"
#include "paths.hpp"
#include "sqlite.hpp"
#include "windows_fs.hpp"
#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <utility>

namespace mini {
namespace {
using namespace detail;
constexpr std::int64_t application_id = 0x4d454958; // MEIX
class Timer {
    double& elapsed_;
    std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};
public:
    /// 绑定要累计的耗时字段，并通过成员初始值记录当前阶段的开始时间。
    explicit Timer(double& elapsed) : elapsed_(elapsed) {}
    /// 将本作用域经过的时间以毫秒累加到绑定字段，便于统计重复操作的总成本。
    ~Timer() { elapsed_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count(); }
};
class ScanControl {
    const ScanOptions& options_;
    const ScanResult& result_;
    std::chrono::steady_clock::time_point last_progress_{};
    std::exception_ptr callback_error_;
public:
    /// 引用本次扫描的回调配置和实时计数，不拥有数据库连接。
    ScanControl(const ScanOptions& options, const ScanResult& result) : options_(options), result_(result) {}
    /// 在 C++ 边界检查取消请求；取消函数抛出的应用异常原样向上传播。
    void check() const { if (options_.cancelled && options_.cancelled()) throw ScanCancelled(); }
    /// 强制发送阶段边界，普通扫描进度按连接本次配置节流，回调只在提交前执行。
    void report(ScanPhase phase, bool force = false) {
        if (!options_.progress) return;
        const auto now = std::chrono::steady_clock::now();
        if (!force && std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_).count() < options_.progress_interval_ms) return;
        options_.progress({phase, result_.files, result_.directories, result_.skipped});
        last_progress_ = now;
    }
    /// SQLite 在长 SQL 内轮询此 C 回调；捕获所有异常并请求中断，绝不让异常越过 C 边界。
    static int poll(void* state) noexcept {
        auto& control = *static_cast<ScanControl*>(state);
        try { control.check(); return 0; }
        catch (...) { control.callback_error_ = std::current_exception(); return 1; }
    }
    /// SQL 中断后在已移除 C 回调的 C++ 边界重抛原始取消或应用异常。
    void rethrow_callback_error() const { if (callback_error_) std::rethrow_exception(callback_error_); }
};
class ScanSqlHandler {
    sqlite3* db_;
public:
    /// 在写事务之后安装连接局部处理器，确保大 SQL 同样可取消。
    ScanSqlHandler(Database& db, ScanControl& control) : db_(db.handle) {
        sqlite3_progress_handler(db_, 1000, ScanControl::poll, &control);
    }
    /// 析构先于写事务回滚，防止取消处理器中断清理操作。
    ~ScanSqlHandler() { remove(); }
    /// 提交或传播异常前移除处理器；重复调用无害且不执行应用回调。
    void remove() noexcept { sqlite3_progress_handler(db_, 0, nullptr, nullptr); }
};
#ifndef _WIN32
struct EntryMetadata {
    fs::path path;
    bool directory{};
    bool skipped{};
    std::uint64_t size{};
    std::int64_t modified_at{};
};
/// 在非 Windows 平台读取类型、大小及修改时间，目录大小记为 0，读取失败时抛出异常。
EntryMetadata read_metadata(const fs::path& path) {
    const bool directory = fs::is_directory(path);
    const auto modified = fs::file_time_type::clock::to_sys(fs::last_write_time(path));
    return {path, directory, false, directory ? 0 : fs::file_size(path),
        std::chrono::duration_cast<std::chrono::seconds>(modified.time_since_epoch()).count()};
}
#endif

/// 执行查询并返回首行首列的整数值，用于读取数据库格式标记；没有结果行时抛错。
std::int64_t scalar(Database& db, const char* sql) {
    Statement stmt(db, sql);
    if (!stmt.row()) throw std::runtime_error("Missing database metadata");
    return stmt.number(0);
}

/// 校验 application_id 和格式版本；create 为 true 时允许为空库建立 schema 1。
/// 外部数据库或不兼容的格式会被拒绝，防止扫描覆盖其他应用的数据。
void schema(Database& db, bool create) {
    const auto app = scalar(db, "PRAGMA application_id");
    const auto version = scalar(db, "PRAGMA user_version");
    if (app == application_id && version == 1) return;
    if (!create || app != 0 || version != 0 ||
        scalar(db, "SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'") != 0)
        throw std::runtime_error("Not a supported MiniEverything index (schema version 1 required)");
    db.exec(R"sql(
        CREATE TABLE roots (
            id INTEGER PRIMARY KEY,
            path TEXT NOT NULL UNIQUE,
            scanned_at INTEGER NOT NULL DEFAULT 0
        );
        CREATE TABLE nodes (
            id INTEGER PRIMARY KEY,
            root_id INTEGER NOT NULL REFERENCES roots(id),
            parent_id INTEGER REFERENCES nodes(id),
            name TEXT NOT NULL,
            full_path TEXT NOT NULL UNIQUE,
            extension TEXT NOT NULL,
            is_directory INTEGER NOT NULL CHECK(is_directory IN (0,1)),
            size INTEGER NOT NULL CHECK(size >= 0),
            modified_at INTEGER NOT NULL
        );
        CREATE INDEX idx_nodes_root ON nodes(root_id);
        CREATE INDEX idx_nodes_parent ON nodes(parent_id);
        CREATE INDEX idx_nodes_name ON nodes(name COLLATE NOCASE);
        CREATE INDEX idx_nodes_extension ON nodes(extension);
        PRAGMA user_version=1;
    )sql");
    db.exec("PRAGMA application_id=" + std::to_string(application_id));
}

/// 转义 LIKE 的通配符与转义符，再包上百分号，使用户文本按字面子串进行匹配。
std::string pattern(const std::string& text) {
    std::string value = "%";
    for (char c : text) {
        if (c == '%' || c == '_' || c == '\\') value += '\\';
        value += c;
    }
    return value + "%";
}
} // namespace

/// 规范化路径并检查范围，以迭代 DFS 在单个写事务中重建或增量校准当前根。
/// 复用 SQL 语句、检查数据库文件身份并累计诊断数据，任一未提交阶段失败都会回滚。
ScanResult scan(const std::filesystem::path& root_path, const std::filesystem::path& database, const ScanOptions& options) {
    using namespace detail;
    if (options.db_cache_mib < 1 || options.db_cache_mib > 1024)
        throw std::invalid_argument("Database cache must be between 1 and 1024 MiB");
    if (options.progress_interval_ms < 0 || options.expected_root_id < 0)
        throw std::invalid_argument("Progress interval and expected root ID must not be negative");
    ScanResult result;
    ScanControl control(options, result);
    control.report(ScanPhase::preparing, true);
    control.check();
    const auto root = normalized(root_path);
    const auto db_path = normalized(database);
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        throw std::invalid_argument("Scan root must be an existing directory");
#else
    if (!fs::is_directory(root)) throw std::invalid_argument("Scan root must be an existing directory");
#endif
    if (contains(root, db_path)) throw std::invalid_argument("Database must be outside the scanned directory");
    const auto prepare_start = std::chrono::steady_clock::now();
    Database db(utf8(db_path), true, options.db_cache_mib);
    result.timings.open_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepare_start).count();
    result.database_stats.cache_mib = options.db_cache_mib;
#ifdef _WIN32
    const auto guard_start = std::chrono::steady_clock::now();
    IndexFileGuard guard(db_path, is_local_drive_path(root) && is_local_drive_path(db_path));
    result.timings.identity_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - guard_start).count();
#endif
    const auto transaction_start = std::chrono::steady_clock::now();
    Transaction tx(db);
    ScanSqlHandler handler(db, control);
    try {
        control.check();
        result.timings.transaction_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - transaction_start).count();
        { Timer timer(result.timings.schema_ms); schema(db, true); }

        std::int64_t root_id = 0;
        bool replacing = false;
        {
            Timer timer(result.timings.roots_ms);
            Statement roots(db, "SELECT id,path FROM roots");
            while (roots.row()) {
                control.check();
                const auto existing = from_utf8(roots.text(1));
                const bool a = contains(root, existing), b = contains(existing, root);
                if (a && b) root_id = roots.number(0);
                else if (a || b) throw std::invalid_argument("Overlapping index roots are not supported");
            }
            if (options.expected_root_id > 0 && root_id != options.expected_root_id)
                throw std::invalid_argument("Expected registered index root no longer matches the scan path");
            if (root_id == 0) {
                Statement add(db, "INSERT INTO roots(path) VALUES(?)");
                add.bind(1, utf8(root)); add.run(); root_id = sqlite3_last_insert_rowid(db.handle);
            } else {
                replacing = true;
                // 等价根路径仍可能改变大小写拼写；与节点校准同事务同步注册路径，并保留原根 ID。
                Statement synchronize(db, "UPDATE roots SET path=? WHERE id=? AND path<>?");
                const auto path_text = utf8(root);
                synchronize.bind(1, path_text); synchronize.bind(2, root_id); synchronize.bind(3, path_text);
                synchronize.run();
            }
        }
        if (replacing && !options.incremental) {
            Timer timer(result.timings.clear_ms);
            control.check();
            Statement clear(db, "DELETE FROM nodes WHERE root_id=?");
            clear.bind(1, root_id); clear.run();
            result.removed = sqlite3_changes64(db.handle);
        }

        const auto statements_start = std::chrono::steady_clock::now();
        Statement insert(db, R"sql(INSERT INTO nodes
            (root_id,parent_id,name,full_path,extension,is_directory,size,modified_at)
            VALUES(?,?,?,?,?,?,?,?))sql");
        std::unique_ptr<Statement> lookup, update, seen;
        if (options.incremental) {
            db.exec("CREATE TEMP TABLE scan_seen(id INTEGER PRIMARY KEY)");
            lookup = std::make_unique<Statement>(db, "SELECT id,coalesce(parent_id,0),name,extension,is_directory,size,modified_at FROM nodes WHERE full_path=? AND root_id=?");
            update = std::make_unique<Statement>(db, "UPDATE nodes SET parent_id=?,name=?,extension=?,is_directory=?,size=?,modified_at=? WHERE id=?");
            seen = std::make_unique<Statement>(db, "INSERT INTO scan_seen(id) VALUES(?)");
        }
        result.timings.statements_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - statements_start).count();
        result.timings.prepare_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepare_start).count()
            - result.timings.identity_ms;
        // 逐路径查找旧节点，只有元数据变化时更新；返回稳定 ID，并把本次节点登记到临时表。
        auto add_node = [&](const EntryMetadata& metadata, std::int64_t parent) {
            control.check();
            Timer timer(result.timings.write_ms);
            const auto& path = metadata.path;
            const bool directory = metadata.directory;
            const auto bytes = directory ? 0 : metadata.size;
            if (bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                throw std::runtime_error("File size exceeds index range");
            auto name = path.filename();
            if (name.empty()) name = path;
            const auto node_name = utf8(name), node_path = utf8(path);
            const auto extension = directory ? std::string{} : ascii_lower(utf8(path.extension()));
            std::int64_t id = 0;
            if (lookup) {
                lookup->bind(1, node_path); lookup->bind(2, root_id);
                if (lookup->row()) {
                    id = lookup->number(0);
                    const bool changed = lookup->number(1) != parent || lookup->text(2) != node_name || lookup->text(3) != extension ||
                        lookup->number(4) != static_cast<std::int64_t>(directory) || lookup->number(5) != static_cast<std::int64_t>(bytes) || lookup->number(6) != metadata.modified_at;
                    lookup->reset();
                    if (changed) {
                        if (parent == 0) update->null(1); else update->bind(1, parent);
                        update->bind(2, node_name); update->bind(3, extension); update->bind(4, static_cast<std::int64_t>(directory));
                        update->bind(5, static_cast<std::int64_t>(bytes)); update->bind(6, metadata.modified_at); update->bind(7, id);
                        update->run(); update->reset(); ++result.updated;
                    } else ++result.unchanged;
                } else lookup->reset();
            }
            if (id == 0) {
                insert.bind(1, root_id);
                if (parent == 0) insert.null(2); else insert.bind(2, parent);
                insert.bind(3, node_name); insert.bind(4, node_path); insert.bind(5, extension);
                insert.bind(6, static_cast<std::int64_t>(directory));
                insert.bind(7, static_cast<std::int64_t>(bytes)); insert.bind(8, metadata.modified_at);
                insert.run(); id = sqlite3_last_insert_rowid(db.handle); insert.reset(); ++result.inserted;
            }
            if (seen) { seen->bind(1, id); seen->run(); seen->reset(); }
            if (directory) ++result.directories; else ++result.files;
            return id;
        };

        control.report(ScanPhase::scanning, true);
        control.check();
        EntryMetadata root_metadata;
        { Timer timer(result.timings.metadata_ms); root_metadata = read_metadata(root); }
        const auto root_node = add_node(root_metadata, 0);
        std::vector<std::pair<fs::path, std::int64_t>> pending{{root, root_node}};
        while (!pending.empty()) {
            control.check();
            auto [directory, parent] = std::move(pending.back()); pending.pop_back();
            // 处理一条枚举结果：计数跳过项、检查数据库别名、插入节点，并把子目录加入 DFS 待办栈。
            auto consume = [&](const EntryMetadata& metadata) {
                control.check();
                if (metadata.skipped) { ++result.skipped; control.report(ScanPhase::scanning); return; }
                if (!metadata.directory) {
    #ifdef _WIN32
                    if (guard.needs_checks()) {
                        Timer timer(result.timings.identity_ms);
                        ++result.identity_checks;
                        if (guard.matches(metadata.path))
                            throw std::invalid_argument("Scan contains a hard link to the index database");
                    }
    #else
                    Timer timer(result.timings.identity_ms);
                    ++result.identity_checks;
                    if (fs::equivalent(metadata.path, db_path))
                        throw std::invalid_argument("Scan contains a hard link to the index database");
    #endif
                }
                const auto id = add_node(metadata, parent);
                if (metadata.directory) pending.emplace_back(metadata.path, id);
                control.report(ScanPhase::scanning);
            };
    #ifdef _WIN32
            const auto cursor_start = std::chrono::steady_clock::now();
            DirectoryCursor cursor(directory);
            result.timings.metadata_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cursor_start).count();
            for (;;) {
                control.check();
                EntryMetadata entry;
                bool found;
                { Timer timer(result.timings.metadata_ms); found = cursor.next(entry); }
                if (!found) break;
                consume(entry);
            }
    #else
            // The portable path retains strict failure and rollback behavior.
            for (const auto& entry : fs::directory_iterator(directory)) {
                control.check();
                const auto& path = entry.path();
                if (is_link(path)) { ++result.skipped; control.report(ScanPhase::scanning); continue; }
                const auto status = entry.status();
                const bool dir = fs::is_directory(status);
                if (!dir && !fs::is_regular_file(status)) { ++result.skipped; control.report(ScanPhase::scanning); continue; }
                EntryMetadata metadata;
                { Timer timer(result.timings.metadata_ms); metadata = read_metadata(path); }
                consume(metadata);
            }
    #endif
        }
        if (options.incremental) {
            Timer timer(result.timings.clear_ms);
            control.check();
            Statement clear(db, "DELETE FROM nodes WHERE root_id=? AND id NOT IN (SELECT id FROM scan_seen)");
            clear.bind(1, root_id); clear.run(); result.removed = sqlite3_changes64(db.handle);
        }
    #ifdef _WIN32
        { Timer timer(result.timings.identity_ms); guard.verify_unchanged(); }
    #endif
        Statement stamp(db, "UPDATE roots SET scanned_at=unixepoch() WHERE id=?");
        stamp.bind(1, root_id); stamp.run();
        control.report(ScanPhase::committing, true);
        control.check();
        handler.remove();
        { Timer timer(result.timings.commit_ms); tx.commit(); }
        result.database_stats.cache_hits = db.cache_stat(SQLITE_DBSTATUS_CACHE_HIT);
        result.database_stats.cache_misses = db.cache_stat(SQLITE_DBSTATUS_CACHE_MISS);
        result.database_stats.cache_writes = db.cache_stat(SQLITE_DBSTATUS_CACHE_WRITE);
        result.database_stats.cache_spills = db.cache_stat(SQLITE_DBSTATUS_CACHE_SPILL);
        return result;
    } catch (...) {
        handler.remove();
        control.rethrow_callback_error();
        throw;
    }
}

/// 以只读方式校验已有索引，执行带参数绑定的字面子串与扩展名查询，稳定排序后限制数量。
/// 返回持久化节点信息，不要求被索引文件仍在磁盘上；无匹配结果时返回空集合。
std::vector<Node> search(const std::filesystem::path& database, const SearchOptions& options) {
    using namespace detail;
    if (options.limit < 1 || options.limit > 10000) throw std::invalid_argument("Limit must be between 1 and 10000");
    if (options.text.find('\0') != std::string::npos || options.extension.find('\0') != std::string::npos)
        throw std::invalid_argument("Search input must not contain NUL");
    Database db(utf8(normalized(database)), false);
    schema(db, false);
    auto extension = ascii_lower(options.extension);
    if (!extension.empty() && extension.front() != '.') extension.insert(extension.begin(), '.');
    Statement query(db, R"sql(SELECT id,coalesce(parent_id,0),name,full_path,is_directory,size,modified_at
        FROM nodes WHERE name LIKE ? ESCAPE '\'
        AND (? = '' OR (is_directory = 0 AND extension = ?))
        ORDER BY name COLLATE NOCASE,full_path LIMIT ?)sql");
    query.bind(1, pattern(options.text)); query.bind(2, extension); query.bind(3, extension);
    query.bind(4, static_cast<std::int64_t>(options.limit));
    std::vector<Node> result;
    while (query.row()) result.push_back({query.number(0), query.number(1), query.text(2), query.text(3),
        query.number(4) != 0, query.number(5), query.number(6)});
    return result;
}
} // namespace mini
