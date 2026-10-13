#include "mini/watch.hpp"
#include "mini/catalog.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
namespace fs = std::filesystem;
/// 检查事件/轮询更新、有限退出、根取消注册与取消行为，仅修改随机临时目录。
int main() {
    const auto base = fs::temp_directory_path() / ("mini-watch-" + std::to_string(std::random_device{}()));
    const auto root = base / "root", database = base / "index.db";
    fs::create_directories(root);
    try {
        mini::scan(root, database);
        const auto id = mini::list_roots(database).at(0).id;
        for (const bool poll : {true, false}) {
            mini::WatchOptions options;
            options.interval_ms=20; options.max_updates=2; options.force_polling=poll;
            int updates=0;
            // 初次校准完成后创建文件，后续校准必须将其纳入数据库。
            options.on_update = [&](const mini::ScanResult&) {
                if (++updates == 1) { std::ofstream out(root / (poll ? "poll.txt" : "native.txt")); out << "hello"; }
            };
            const auto started = std::chrono::steady_clock::now();
            // 为失效监听设置测试截止期限，避免环境异常造成无限等待。
            options.scan_options.cancelled = [&] { return std::chrono::steady_clock::now()-started > std::chrono::seconds(10); };
            const auto result=mini::watch(database,id,options);
            if(result.updates!=2 || mini::search(database,{poll ? "poll.txt" : "native.txt"}).size()!=1)
                throw std::runtime_error("watch missed file change");
        }
        mini::WatchOptions cancel;
        // 始终取消，确认监听不会开始新的索引校准。
        cancel.scan_options.cancelled = [] { return true; };
        bool cancelled=false;
        try { mini::watch(database,id,cancel); } catch(const mini::ScanCancelled&) { cancelled=true; }
        if(!cancelled) throw std::runtime_error("watch ignored cancellation");
        for (const bool replace : {false, true}) {
            mini::WatchOptions moved;
            moved.interval_ms=1000;
            const auto started=std::chrono::steady_clock::now();
            // 截止期限只防止测试挂起；根失效必须以操作错误而不是取消结束。
            moved.scan_options.cancelled=[&] { return std::chrono::steady_clock::now()-started>std::chrono::seconds(2); };
            // 首次提交后改名根目录，或在原路径建立另一目录，模拟句柄仍指向旧根。
            moved.on_update=[&](const mini::ScanResult&) {
                fs::rename(root,base/"moved");
                if(replace) fs::create_directory(root);
            };
            bool invalidated=false;
            try { mini::watch(database,id,moved); }
            catch(const mini::ScanCancelled&) { throw std::runtime_error("watch missed renamed/replaced physical root"); }
            catch(const std::exception&) { invalidated=true; }
            if(!invalidated) throw std::runtime_error("watch accepted a different physical root");
            if(replace) fs::remove(root);
            fs::rename(base/"moved",root);
            if(!mini::check_index(database).ok) throw std::runtime_error("root failure damaged committed index");
        }
#ifdef _WIN32
        mini::WatchOptions recased;
        const auto recased_started=std::chrono::steady_clock::now();
        // 大小写更名不改变文件 ID，也没有子树事件；必须校验当前规范路径。
        recased.on_update=[&](const mini::ScanResult&) {
            fs::rename(root,base/"staging");
            fs::rename(base/"staging",base/"ROOT");
        };
        // 防止原生监听没有发现大小写更名时让回归测试无限等待。
        recased.scan_options.cancelled=[&] { return std::chrono::steady_clock::now()-recased_started>std::chrono::seconds(2); };
        bool recased_error=false;
        try { mini::watch(database,id,recased); }
        catch(const mini::ScanCancelled&) { throw std::runtime_error("watch missed root case rename"); }
        catch(const std::exception&) { recased_error=true; }
        if(!recased_error) throw std::runtime_error("watch accepted changed canonical path");
        fs::rename(base/"ROOT",base/"staging");
        fs::rename(base/"staging",root);
#endif
        mini::WatchOptions forgotten;
        forgotten.force_polling=true; forgotten.interval_ms=10;
        // 首次校准后取消注册，监听不得重新添加被移除的根。
        forgotten.on_update = [&](const mini::ScanResult&) { mini::forget_root(database,id); };
        bool rejected=false;
        try { mini::watch(database,id,forgotten); } catch(const std::invalid_argument&) { rejected=true; }
        if(!rejected || !mini::list_roots(database).empty()) throw std::runtime_error("watch resurrected root");
        fs::remove_all(base); std::cout << "PASS watch native/poll/cancel/forget\n";
    } catch(const std::exception& e) { std::error_code ec;fs::remove_all(base,ec);std::cerr << e.what() << '\n';return 1; }
}
