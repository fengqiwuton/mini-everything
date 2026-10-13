#include "../src/read_index.hpp"
#include <filesystem>
#include <iostream>
#include <random>
namespace fs = std::filesystem;
/// 检查共享读事务的格式拒绝、只读连接与资源释放，所有文件均属于随机临时夹具。
int main() {
    const auto path = fs::temp_directory_path() / ("mini-read-" + std::to_string(std::random_device{}()) + ".db");
    try {
        {
            mini::detail::Database db(mini::detail::utf8(path), true);
            db.exec("PRAGMA application_id=1296386392; PRAGMA user_version=1; CREATE TABLE data(value); INSERT INTO data VALUES(7);");
        }
        {
            mini::detail::ReadIndex snapshot(path);
            mini::detail::Statement s(snapshot.db, "SELECT value FROM data");
            if (!s.row() || s.number(0) != 7) throw std::runtime_error("missing snapshot data");
            bool rejected = false;
            try { snapshot.db.exec("INSERT INTO data VALUES(8)"); } catch (const std::exception&) { rejected = true; }
            if (!rejected) throw std::runtime_error("read snapshot allowed write");
        }
        {
            mini::detail::Database db(mini::detail::utf8(path), true);
            db.exec("PRAGMA user_version=99;");
        }
        bool rejected = false;
        try { mini::detail::ReadIndex snapshot(path); } catch (const std::exception&) { rejected = true; }
        if (!rejected) throw std::runtime_error("unsupported version accepted");
        fs::remove(path);
        std::cout << "PASS read_index\n";
    } catch (const std::exception& e) { std::error_code ec; fs::remove(path,ec); std::cerr << e.what() << '\n'; return 1; }
}
