#include "mini/index.hpp"
#include <sqlite3.h>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>

namespace fs = std::filesystem;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F action) {
    bool thrown = false;
    try { action(); } catch (const std::exception&) { thrown = true; }
    require(thrown, "operation should reject invalid input");
}
struct Fixture {
    fs::path base = fs::temp_directory_path() / ("mini-test-" + std::to_string(std::random_device{}()));
    fs::path root = base / "root";
    fs::path db = base / "index.db";
    Fixture() { fs::create_directories(root); }
    ~Fixture() { std::error_code ec; fs::remove_all(base, ec); }
    void file(const fs::path& name, const std::string& data = "abc") {
        fs::create_directories((root / name).parent_path());
        std::ofstream out(root / name, std::ios::binary);
        out << data;
        if (!out) throw std::runtime_error("fixture write failed");
    }
};
void sql(const fs::path& file, const char* statement) {
    sqlite3* db = nullptr;
    const auto utf8 = file.u8string();
    require(sqlite3_open(reinterpret_cast<const char*>(utf8.c_str()), &db) == SQLITE_OK, "fixture db open failed");
    const int result = sqlite3_exec(db, statement, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    require(result == SQLITE_OK, "fixture SQL failed");
}

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
void persistence() {
    Fixture f; f.file("offline.txt");
    mini::scan(f.root, f.db);
    fs::remove_all(f.root);
    require(mini::search(f.db, {"offline"}).size() == 1, "search should use persisted index");
}
void rescan() {
    Fixture f; f.file("old.txt"); mini::scan(f.root, f.db);
    fs::remove(f.root / "old.txt"); f.file("new.txt", "123456");
    mini::scan(f.root, f.db); mini::scan(f.root, f.db);
    require(mini::search(f.db, {"old"}).empty(), "stale record survives rescan");
    const auto found = mini::search(f.db, {"new"});
    require(found.size() == 1 && found[0].size == 6, "rescan duplicated or lost metadata");
    require(mini::search(f.db, {""}).size() == 2, "unexpected duplicate nodes");
}
void literal_search() {
    Fixture f; f.file("100%_report.txt"); f.file("100xxreport.txt"); f.file("O'Brien.txt");
    mini::scan(f.root, f.db);
    require(mini::search(f.db, {"%_"}).size() == 1, "LIKE wildcards not escaped");
    require(mini::search(f.db, {"O'Brien"}).size() == 1, "quote handling incorrect");
    require(mini::search(f.db, {"' OR 1=1 --"}).empty(), "SQL injection");
}
void filters() {
    Fixture f; f.file("REPORT.PDF"); f.file("report.txt"); f.file("another.pdf");
    fs::create_directory(f.root / "folder.pdf"); mini::scan(f.root, f.db);
    auto found = mini::search(f.db, {"report", ".PDF", 100});
    require(found.size() == 1 && found[0].name == "REPORT.PDF", "case or extension filtering failed");
    require(mini::search(f.db, {"", "pdf"}).size() == 2, "extension filter includes directory");
    require(mini::search(f.db, {"", "", 1}).size() == 1, "limit not enforced");
    rejects([&] { mini::search(f.db, {"", "", 0}); });
    rejects([&] { mini::search(f.db, {"", "", 10001}); });
}
void unicode() {
    Fixture f; f.file(fs::path(u8"资料 空间/年度报告.txt"));
    f.db = f.base / fs::path(u8"中文索引.db"); mini::scan(f.root, f.db);
    const auto found = mini::search(f.db, {"年度报告"});
    require(found.size() == 1 && found[0].name == "年度报告.txt", "UTF-8 round trip failed");
}
void overlap() {
    Fixture f; f.file("child/a.txt"); mini::scan(f.root, f.db);
    rejects([&] { mini::scan(f.root / "child", f.db); });
    const auto sibling = f.base / "root-sibling";
    fs::create_directory(sibling); mini::scan(sibling, f.db);
    require(mini::search(f.db, {"a.txt"}).size() == 1, "second root erased first");
    // Ancestor rejection with database outside both roots.
    Fixture g; g.file("child/a.txt"); mini::scan(g.root / "child", g.db);
    rejects([&] { mini::scan(g.root, g.db); });
}
void invalid_root() {
    Fixture f; f.file("a.txt"); mini::scan(f.root, f.db);
    rejects([&] { mini::scan(f.root / "missing", f.db); });
    rejects([&] { mini::scan(f.root / "a.txt", f.db); });
    require(mini::search(f.db, {"a.txt"}).size() == 1, "invalid scan destroyed index");
    rejects([&] { mini::search(f.base / "missing.db", {"a"}); });
    require(!fs::exists(f.base / "missing.db"), "search silently created db");
}
void database_inside() {
    Fixture f;
    rejects([&] { mini::scan(f.root, f.root / "index.db"); });
    require(!fs::exists(f.root / "index.db"), "unsafe database created");
}
void deep_tree() {
    Fixture f; fs::path p;
    for (int i = 0; i < 35; ++i) p /= "d";
    f.file(p / "leaf.txt");
    const auto result = mini::scan(f.root, f.db);
    require(result.directories == 36 && result.files == 1, "deep traversal failed");
}
void rollback() {
    Fixture f; f.file("old.txt"); mini::scan(f.root, f.db);
    // A real SQLite failure after the transaction has started must restore old nodes.
    sql(f.db, "CREATE TRIGGER fail_insert BEFORE INSERT ON nodes BEGIN SELECT RAISE(ABORT, 'test disk failure'); END;");
    fs::remove(f.root / "old.txt"); f.file("new.txt");
    rejects([&] { mini::scan(f.root, f.db); });
    require(mini::search(f.db, {"old.txt"}).size() == 1, "failed scan removed old snapshot");
    require(mini::search(f.db, {"new.txt"}).empty(), "failed scan published partial result");
}
void foreign_database() {
    Fixture f; sql(f.db, "CREATE TABLE unrelated(value TEXT);");
    rejects([&] { mini::scan(f.root, f.db); });
    rejects([&] { mini::search(f.db, {""}); });
    sql(f.db, "INSERT INTO unrelated VALUES ('still usable');");
}
void schema_version() {
    Fixture f; f.file("report.txt"); mini::scan(f.root, f.db);
    sql(f.db, "PRAGMA user_version=99;");
    rejects([&] { mini::search(f.db, {""}); });
    rejects([&] { mini::scan(f.root, f.db); });
    sql(f.db, "PRAGMA user_version=1;");
    require(mini::search(f.db, {"report.txt"}).size() == 1, "unsupported schema was modified");
}
void database_hardlink() {
    Fixture f; f.file("old.txt"); mini::scan(f.root, f.db);
    fs::create_hard_link(f.db, f.root / "alias.db");
    rejects([&] { mini::scan(f.root, f.db); });
    require(mini::search(f.db, {"alias.db"}).empty(), "database hardlink was indexed");
    require(mini::search(f.db, {"old.txt"}).size() == 1, "hardlink rejection lost old index");
}

int main(int argc, char** argv) {
    const std::map<std::string, std::function<void()>> cases = {
        {"scan_tree", scan_tree}, {"persistence", persistence}, {"rescan", rescan},
        {"literal_search", literal_search}, {"filters", filters}, {"unicode", unicode},
        {"overlap", overlap}, {"invalid_root", invalid_root}, {"database_inside", database_inside},
        {"deep_tree", deep_tree}, {"rollback", rollback}, {"foreign_database", foreign_database},
        {"schema_version", schema_version}, {"database_hardlink", database_hardlink}};
    try {
        require(argc == 2, "expected case name"); cases.at(argv[1])();
        std::cout << "PASS " << argv[1] << '\n'; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
