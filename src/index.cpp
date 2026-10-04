#include "mini/index.hpp"
#include "paths.hpp"
#include "sqlite.hpp"
#include "windows_fs.hpp"
#include <chrono>
#include <limits>
#include <utility>

namespace mini {
namespace {
using namespace detail;
constexpr std::int64_t application_id = 0x4d454958; // MEIX
class Timer {
    double& elapsed_;
    std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};
public:
    explicit Timer(double& elapsed) : elapsed_(elapsed) {}
    ~Timer() { elapsed_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count(); }
};
#ifndef _WIN32
struct EntryMetadata {
    fs::path path;
    bool directory{};
    bool skipped{};
    std::uint64_t size{};
    std::int64_t modified_at{};
};
EntryMetadata read_metadata(const fs::path& path) {
    const bool directory = fs::is_directory(path);
    const auto modified = fs::file_time_type::clock::to_sys(fs::last_write_time(path));
    return {path, directory, false, directory ? 0 : fs::file_size(path),
        std::chrono::duration_cast<std::chrono::seconds>(modified.time_since_epoch()).count()};
}
#endif

std::int64_t scalar(Database& db, const char* sql) {
    Statement stmt(db, sql);
    if (!stmt.row()) throw std::runtime_error("Missing database metadata");
    return stmt.number(0);
}

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

std::string pattern(const std::string& text) {
    std::string value = "%";
    for (char c : text) {
        if (c == '%' || c == '_' || c == '\\') value += '\\';
        value += c;
    }
    return value + "%";
}
} // namespace

ScanResult scan(const std::filesystem::path& root_path, const std::filesystem::path& database, const ScanOptions& options) {
    using namespace detail;
    if (options.db_cache_mib < 1 || options.db_cache_mib > 1024)
        throw std::invalid_argument("Database cache must be between 1 and 1024 MiB");
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
    ScanResult result;
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
    result.timings.transaction_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - transaction_start).count();
    { Timer timer(result.timings.schema_ms); schema(db, true); }

    std::int64_t root_id = 0;
    bool replacing = false;
    {
        Timer timer(result.timings.roots_ms);
        Statement roots(db, "SELECT id,path FROM roots");
        while (roots.row()) {
            const auto existing = from_utf8(roots.text(1));
            const bool a = contains(root, existing), b = contains(existing, root);
            if (a && b) root_id = roots.number(0);
            else if (a || b) throw std::invalid_argument("Overlapping index roots are not supported");
        }
        if (root_id == 0) {
            Statement add(db, "INSERT INTO roots(path) VALUES(?)");
            add.bind(1, utf8(root)); add.run(); root_id = sqlite3_last_insert_rowid(db.handle);
        } else replacing = true;
    }
    if (replacing) {
        Timer timer(result.timings.clear_ms);
        Statement clear(db, "DELETE FROM nodes WHERE root_id=?");
        clear.bind(1, root_id); clear.run();
    }

    const auto statements_start = std::chrono::steady_clock::now();
    Statement insert(db, R"sql(INSERT INTO nodes
        (root_id,parent_id,name,full_path,extension,is_directory,size,modified_at)
        VALUES(?,?,?,?,?,?,?,?))sql");
    result.timings.statements_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - statements_start).count();
    result.timings.prepare_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepare_start).count()
        - result.timings.identity_ms;
    auto add_node = [&](const EntryMetadata& metadata, std::int64_t parent) {
        Timer timer(result.timings.write_ms);
        const auto& path = metadata.path;
        const bool directory = metadata.directory;
        const auto bytes = directory ? 0 : metadata.size;
        if (bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::runtime_error("File size exceeds index range");
        auto name = path.filename();
        if (name.empty()) name = path;
        insert.bind(1, root_id);
        if (parent == 0) insert.null(2); else insert.bind(2, parent);
        insert.bind(3, utf8(name)); insert.bind(4, utf8(path));
        insert.bind(5, directory ? "" : ascii_lower(utf8(path.extension())));
        insert.bind(6, static_cast<std::int64_t>(directory));
        insert.bind(7, static_cast<std::int64_t>(bytes)); insert.bind(8, metadata.modified_at);
        insert.run();
        const auto id = sqlite3_last_insert_rowid(db.handle);
        insert.reset();
        if (directory) ++result.directories; else ++result.files;
        return id;
    };

    EntryMetadata root_metadata;
    { Timer timer(result.timings.metadata_ms); root_metadata = read_metadata(root); }
    const auto root_node = add_node(root_metadata, 0);
    std::vector<std::pair<fs::path, std::int64_t>> pending{{root, root_node}};
    while (!pending.empty()) {
        auto [directory, parent] = std::move(pending.back()); pending.pop_back();
        auto consume = [&](const EntryMetadata& metadata) {
            if (metadata.skipped) { ++result.skipped; return; }
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
        };
#ifdef _WIN32
        const auto cursor_start = std::chrono::steady_clock::now();
        DirectoryCursor cursor(directory);
        result.timings.metadata_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cursor_start).count();
        for (;;) {
            EntryMetadata entry;
            bool found;
            { Timer timer(result.timings.metadata_ms); found = cursor.next(entry); }
            if (!found) break;
            consume(entry);
        }
#else
        // The portable path retains strict failure and rollback behavior.
        for (const auto& entry : fs::directory_iterator(directory)) {
            const auto& path = entry.path();
            if (is_link(path)) { ++result.skipped; continue; }
            const auto status = entry.status();
            const bool dir = fs::is_directory(status);
            if (!dir && !fs::is_regular_file(status)) { ++result.skipped; continue; }
            EntryMetadata metadata;
            { Timer timer(result.timings.metadata_ms); metadata = read_metadata(path); }
            consume(metadata);
        }
#endif
    }
#ifdef _WIN32
    { Timer timer(result.timings.identity_ms); guard.verify_unchanged(); }
#endif
    Statement stamp(db, "UPDATE roots SET scanned_at=unixepoch() WHERE id=?");
    stamp.bind(1, root_id); stamp.run();
    { Timer timer(result.timings.commit_ms); tx.commit(); }
    result.database_stats.cache_hits = db.cache_stat(SQLITE_DBSTATUS_CACHE_HIT);
    result.database_stats.cache_misses = db.cache_stat(SQLITE_DBSTATUS_CACHE_MISS);
    result.database_stats.cache_writes = db.cache_stat(SQLITE_DBSTATUS_CACHE_WRITE);
    result.database_stats.cache_spills = db.cache_stat(SQLITE_DBSTATUS_CACHE_SPILL);
    return result;
}

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
