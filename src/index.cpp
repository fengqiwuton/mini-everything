#include "mini/index.hpp"
#include "paths.hpp"
#include "sqlite.hpp"
#include <chrono>
#include <limits>
#include <utility>

namespace mini {
namespace {
using namespace detail;
constexpr std::int64_t application_id = 0x4d454958; // MEIX

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

ScanResult scan(const std::filesystem::path& root_path, const std::filesystem::path& database) {
    using namespace detail;
    const auto root = normalized(root_path);
    const auto db_path = normalized(database);
    if (!fs::is_directory(root)) throw std::invalid_argument("Scan root must be an existing directory");
    if (contains(root, db_path)) throw std::invalid_argument("Database must be outside the scanned directory");
    Database db(utf8(db_path), true);
    Transaction tx(db);
    schema(db, true);

    std::int64_t root_id = 0;
    {
        Statement roots(db, "SELECT id,path FROM roots");
        while (roots.row()) {
            const auto existing = from_utf8(roots.text(1));
            const bool a = contains(root, existing), b = contains(existing, root);
            if (a && b) root_id = roots.number(0);
            else if (a || b) throw std::invalid_argument("Overlapping index roots are not supported");
        }
    }
    if (root_id == 0) {
        Statement add(db, "INSERT INTO roots(path) VALUES(?)");
        add.bind(1, utf8(root)); add.run(); root_id = sqlite3_last_insert_rowid(db.handle);
    } else {
        Statement clear(db, "DELETE FROM nodes WHERE root_id=?");
        clear.bind(1, root_id); clear.run();
    }

    ScanResult result;
    Statement insert(db, R"sql(INSERT INTO nodes
        (root_id,parent_id,name,full_path,extension,is_directory,size,modified_at)
        VALUES(?,?,?,?,?,?,?,?))sql");
    auto add_node = [&](const fs::path& path, std::int64_t parent, bool directory) {
        const auto bytes = directory ? 0 : fs::file_size(path);
        if (bytes > static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::runtime_error("File size exceeds index range");
#ifdef _MSC_VER
        // MSVC file_clock exposes to_utc instead of to_sys.
        const auto modified = std::chrono::clock_cast<std::chrono::system_clock>(fs::last_write_time(path));
#else
        const auto modified = fs::file_time_type::clock::to_sys(fs::last_write_time(path));
#endif
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(modified.time_since_epoch()).count();
        auto name = path.filename();
        if (name.empty()) name = path;
        insert.bind(1, root_id);
        if (parent == 0) insert.null(2); else insert.bind(2, parent);
        insert.bind(3, utf8(name)); insert.bind(4, utf8(path));
        insert.bind(5, directory ? "" : ascii_lower(utf8(path.extension())));
        insert.bind(6, static_cast<std::int64_t>(directory));
        insert.bind(7, static_cast<std::int64_t>(bytes)); insert.bind(8, seconds);
        insert.run();
        const auto id = sqlite3_last_insert_rowid(db.handle);
        insert.reset();
        if (directory) ++result.directories; else ++result.files;
        return id;
    };

    const auto root_node = add_node(root, 0, true);
    std::vector<std::pair<fs::path, std::int64_t>> pending{{root, root_node}};
    while (!pending.empty()) {
        auto [directory, parent] = std::move(pending.back()); pending.pop_back();
        // Throw on unreadable directories; never silently publish an incomplete snapshot.
        for (const auto& entry : fs::directory_iterator(directory)) {
            const auto& path = entry.path();
            if (is_link(path)) { ++result.skipped; continue; }
            const auto status = entry.status();
            const bool dir = fs::is_directory(status);
            if (!dir && !fs::is_regular_file(status)) { ++result.skipped; continue; }
            if (!dir && fs::equivalent(path, db_path))
                throw std::invalid_argument("Scan contains a hard link to the index database");
            const auto id = add_node(path, parent, dir);
            if (dir) pending.emplace_back(path, id);
        }
    }
    Statement stamp(db, "UPDATE roots SET scanned_at=unixepoch() WHERE id=?");
    stamp.bind(1, root_id); stamp.run();
    tx.commit();
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
