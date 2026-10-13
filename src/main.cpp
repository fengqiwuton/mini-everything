#include "mini/catalog.hpp"
#include "mini/query.hpp"
#include "mini/watch.hpp"
#include "cli_output.hpp"
#include "paths.hpp"
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <limits>
#include <set>
#ifdef _WIN32
#include <shellapi.h>
#endif

namespace {
enum class Command { scan, search, roots, stats, check, forget, info, children, ancestors, tree, du, refresh, watch };
struct Options {
    Command command{};
    std::string argument;
    std::string database;
    mini::QueryOptions query;
    mini::ScanOptions scan;
    std::int64_t id{};
    bool json{}, show_ids{}, profile{}, progress{}, polling{};
    int depth{3}, tree_limit{1000}, interval_ms{1000}, updates{}, timeout_ms{};
};

/// 输出版本、所有命令、主要参数范围和退出码，供 help/version 使用。
void help() {
    std::cout << "MiniEverything 0.5.0\n"
        "  mini-everything scan <directory> --db <index.db> [--incremental] [--progress]\n"
        "  mini-everything search <text> --db <index.db> [--ext pdf] [--limit 100]\n"
        "    [--mode substring|exact|prefix] [--type any|file|dir] [--path] [--show-ids]\n"
        "    [--sort name|path|size|mtime] [--desc] [--offset 0..1000000]\n"
        "    [--root <root-id>] [--under <node-id>] [--min-size <bytes>] [--max-size <bytes>]\n"
        "    [--after <unix-seconds>] [--before <unix-seconds>]\n"
        "  mini-everything roots|stats|check --db <index.db>\n"
        "  mini-everything forget <root-id> --db <index.db>\n"
        "  mini-everything info|children|ancestors|tree|du <node-id> --db <index.db>\n"
        "    children: [--limit 100] [--offset 0]; tree: [--depth 3] [--limit 1000]\n"
        "  mini-everything refresh|watch <root-id> --db <index.db>\n"
        "    watch: [--interval-ms 1000] [--updates 0] [--poll]\n"
        "    scan/refresh/watch: [--profile] [--db-cache-mib 64] [--progress] [--timeout-ms 0]\n"
        "  All commands: [--format tsv|json]; watch JSON is one committed update per line.\n"
        "  mini-everything --help | --version\n"
        "Search is a literal substring match (ASCII case-insensitive) by default.\n"
        "Database must be outside the scanned directory. Links are skipped.\n"
        "Scan page-cache target: 1..1024 MiB, default 64 (not total process memory).\n"
        "Tree depth: 0..1000; row limit: 1..10000; timeout: 0..86400000 ms (0 unlimited).\n"
        "Watch interval: 10..3600000 ms; updates: 0..1000000 (0 unlimited).\n"
        "Exit codes: 0 success, 1 operation failed, 2 invalid arguments, 130 cancelled.\n";
}

/// 将命令映射为固定枚举，不把用户命令文本用于 SQL 或其他动态执行。
Command command(const std::string& name) {
    const std::pair<const char*,Command> values[] = {
        {"scan",Command::scan},{"search",Command::search},{"roots",Command::roots},
        {"stats",Command::stats},{"check",Command::check},{"forget",Command::forget},
        {"info",Command::info},{"children",Command::children},{"ancestors",Command::ancestors},
        {"tree",Command::tree},{"du",Command::du},{"refresh",Command::refresh},{"watch",Command::watch}
    };
    for (const auto& [text,value] : values) if (name==text) return value;
    throw std::invalid_argument("Unknown command; use --help");
}
/// 判断命令是否读取一个位置参数，根列表等零参数命令不接受额外位置文本。
bool has_argument(Command value) {
    return value!=Command::roots && value!=Command::stats && value!=Command::check;
}
/// 判断命令是否涉及扫描维护，以统一缓存、进度和取消参数的白名单。
bool maintenance(Command value) {
    return value==Command::scan || value==Command::refresh || value==Command::watch;
}
/// 根据命令生成允许的选项集合，任何其他选项均在访问数据库前被拒绝。
std::set<std::string> allowed_options(Command value) {
    std::set<std::string> flags{"--db","--format"};
    if (maintenance(value)) flags.insert({"--profile","--db-cache-mib","--progress","--timeout-ms"});
    if (value==Command::scan) flags.insert("--incremental");
    if (value==Command::watch) flags.insert({"--interval-ms","--updates","--poll"});
    if (value==Command::search) flags.insert({"--ext","--limit","--offset","--mode","--type","--sort","--desc",
        "--path","--root","--under","--min-size","--max-size","--after","--before","--show-ids"});
    if (value==Command::children) flags.insert({"--limit","--offset"});
    if (value==Command::tree) flags.insert({"--depth","--limit"});
    return flags;
}
/// 完整解析有符号十进制整数并验证上下界，拒绝空值、尾随字符和整数溢出。
std::int64_t integer(const std::string& text, std::int64_t low, std::int64_t high, const std::string& label) {
    std::int64_t value{};
    const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
    if (error!=std::errc{} || end!=text.data()+text.size() || value<low || value>high)
        throw std::invalid_argument(label+" must be an integer between "+std::to_string(low)+" and "+std::to_string(high));
    return value;
}
/// 校验命令位置参数、白名单、重复选项和数值范围，再生成查询与扫描配置。
Options parse(const std::vector<std::string>& args) {
    if (args.empty()) throw std::invalid_argument("Expected a command; use --help");
    Options result; result.command=command(args[0]);
    std::size_t cursor=1;
    if (has_argument(result.command)) {
        if (args.size()<2) throw std::invalid_argument("Missing command argument");
        result.argument=args[1]; cursor=2;
        if (result.command==Command::search) result.query.text=result.argument;
        else if (result.command!=Command::scan) result.id=integer(result.argument,1,std::numeric_limits<std::int64_t>::max(),"ID");
    }
    const auto allowed=allowed_options(result.command);
    const std::set<std::string> switches{"--profile","--incremental","--progress","--show-ids","--desc","--path","--poll"};
    std::set<std::string> seen;
    const auto maximum=std::numeric_limits<std::int64_t>::max();
    const auto minimum=std::numeric_limits<std::int64_t>::min();
    while (cursor<args.size()) {
        const auto& flag=args[cursor++];
        if (!allowed.contains(flag)) throw std::invalid_argument("Unknown option or extra argument: "+flag);
        if (!seen.insert(flag).second) throw std::invalid_argument("Duplicate option: "+flag);
        if (switches.contains(flag)) {
            if (flag=="--profile") result.profile=true;
            else if (flag=="--incremental") result.scan.incremental=true;
            else if (flag=="--progress") result.progress=true;
            else if (flag=="--show-ids") result.show_ids=true;
            else if (flag=="--desc") result.query.descending=true;
            else if (flag=="--path") result.query.match_path=true;
            else if (flag=="--poll") result.polling=true;
            continue;
        }
        if (cursor==args.size()) throw std::invalid_argument("Missing value: "+flag);
        const auto& value=args[cursor++];
        if (flag=="--db") result.database=value;
        else if (flag=="--format") {
            if (value!="tsv" && value!="json") throw std::invalid_argument("--format must be tsv or json");
            result.json=value=="json";
        } else if (flag=="--ext") result.query.extension=value;
        else if (flag=="--limit") {
            const auto limit=static_cast<int>(integer(value,1,10000,flag));
            if (result.command==Command::tree) result.tree_limit=limit; else result.query.limit=limit;
        } else if (flag=="--offset") result.query.offset=static_cast<int>(integer(value,0,1000000,flag));
        else if (flag=="--depth") result.depth=static_cast<int>(integer(value,0,1000,flag));
        else if (flag=="--db-cache-mib") result.scan.db_cache_mib=static_cast<int>(integer(value,1,1024,flag));
        else if (flag=="--timeout-ms") result.timeout_ms=static_cast<int>(integer(value,0,86400000,flag));
        else if (flag=="--interval-ms") result.interval_ms=static_cast<int>(integer(value,10,3600000,flag));
        else if (flag=="--updates") result.updates=static_cast<int>(integer(value,0,1000000,flag));
        else if (flag=="--root") result.query.root_id=integer(value,1,maximum,flag);
        else if (flag=="--under") result.query.under_id=integer(value,1,maximum,flag);
        else if (flag=="--min-size") result.query.min_size=integer(value,0,maximum,flag);
        else if (flag=="--max-size") result.query.max_size=integer(value,0,maximum,flag);
        else if (flag=="--after") result.query.modified_after=integer(value,minimum,maximum,flag);
        else if (flag=="--before") result.query.modified_before=integer(value,minimum,maximum,flag);
        else if (flag=="--mode") {
            if (value=="substring") result.query.mode=mini::MatchMode::substring;
            else if (value=="exact") result.query.mode=mini::MatchMode::exact;
            else if (value=="prefix") result.query.mode=mini::MatchMode::prefix;
            else throw std::invalid_argument("--mode must be substring, exact or prefix");
        } else if (flag=="--type") {
            if (value=="any") result.query.kind=mini::NodeKind::any;
            else if (value=="file") result.query.kind=mini::NodeKind::file;
            else if (value=="dir") result.query.kind=mini::NodeKind::directory;
            else throw std::invalid_argument("--type must be any, file or dir");
        } else if (flag=="--sort") {
            if (value=="name") result.query.sort=mini::SortField::name;
            else if (value=="path") result.query.sort=mini::SortField::path;
            else if (value=="size") result.query.sort=mini::SortField::size;
            else if (value=="mtime") result.query.sort=mini::SortField::modified;
            else throw std::invalid_argument("--sort must be name, path, size or mtime");
        }
    }
    if (result.database.empty()) throw std::invalid_argument("--db is required");
    if (result.query.min_size && result.query.max_size && *result.query.min_size>*result.query.max_size)
        throw std::invalid_argument("Minimum size exceeds maximum size");
    if (result.query.modified_after && result.query.modified_before && *result.query.modified_after>*result.query.modified_before)
        throw std::invalid_argument("Modified lower bound exceeds upper bound");
    return result;
}

#ifdef _WIN32
std::atomic<bool> interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free);
/// Windows 控制台处理器仅记录 Ctrl+C/Ctrl+Break，不执行 IO 或调用扫描逻辑。
BOOL WINAPI interrupt_handler(DWORD event) {
    if (event!=CTRL_C_EVENT && event!=CTRL_BREAK_EVENT) return FALSE;
    interrupted.store(true,std::memory_order_relaxed); return TRUE;
}
#else
volatile std::sig_atomic_t interrupted=0;
/// POSIX 信号处理器仅写入 sig_atomic_t 标志，取消检查由正常执行路径完成。
void interrupt_handler(int) { interrupted=1; }
#endif
class InterruptGuard {
#ifndef _WIN32
    using Handler=void(*)(int);
    Handler previous_{};
#endif
public:
    /// 为本次维护命令安装取消处理器，失败时明确报错。
    InterruptGuard() {
#ifdef _WIN32
        interrupted.store(false,std::memory_order_relaxed);
        if (!SetConsoleCtrlHandler(interrupt_handler,TRUE)) throw std::runtime_error("Cannot install console cancellation handler");
#else
        interrupted=0;
        previous_=std::signal(SIGINT,interrupt_handler);
        if (previous_==SIG_ERR) throw std::runtime_error("Cannot install interrupt handler");
#endif
    }
    /// 命令结束时移除或恢复取消处理器，析构不传播异常。
    ~InterruptGuard() {
#ifdef _WIN32
        SetConsoleCtrlHandler(interrupt_handler,FALSE);
#else
        std::signal(SIGINT,previous_);
#endif
    }
    /// 禁止复制取消处理器所有权，确保处理器只由当前作用域恢复。
    InterruptGuard(const InterruptGuard&)=delete;
    /// 禁止赋值覆盖处理器状态。
    InterruptGuard& operator=(const InterruptGuard&)=delete;
};
/// 在正常执行路径读取平台取消标志，信号回调本身不做任何额外工作。
bool was_interrupted() {
#ifdef _WIN32
    return interrupted.load(std::memory_order_relaxed);
#else
    return interrupted!=0;
#endif
}
/// 把固定扫描阶段枚举转换为进度诊断名称。
const char* phase_name(mini::ScanPhase phase) {
    switch (phase) {
    case mini::ScanPhase::preparing: return "preparing";
    case mini::ScanPhase::scanning: return "scanning";
    case mini::ScanPhase::committing: return "committing";
    }
    return "unknown";
}
/// 按持久化根 ID 找到其注册路径，防止 refresh 将节点 ID 或失效根误当作扫描根。
std::filesystem::path registered_root(const std::filesystem::path& database, std::int64_t id) {
    for (const auto& root : mini::list_roots(database)) if (root.id==id) return mini::detail::from_utf8(root.path);
    throw std::invalid_argument("Root ID is not registered");
}
/// 为扫描维护绑定进度、Ctrl+C 和批处理截止时间，并在成功提交后输出结果。
void run_maintenance(Options& options, const std::filesystem::path& database) {
    InterruptGuard interrupt_guard;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (options.timeout_ms) deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(options.timeout_ms);
    // 取消回调仅查看取消标志和单调时钟，不做 IO 或写数据库。
    options.scan.cancelled=[deadline] {
        return was_interrupted() || (deadline && std::chrono::steady_clock::now()>=*deadline);
    };
    if (options.progress) {
        // 进度通过 stderr 输出，不影响 TSV、JSON 或 watch 的逐行 JSON 数据。
        options.scan.progress=[](const mini::ScanProgress& value) {
            std::cerr << "Progress: phase=" << phase_name(value.phase) << " files=" << value.files
                << " directories=" << value.directories << " skipped=" << value.skipped << '\n';
        };
    }
    if (options.command==Command::watch) {
        mini::WatchOptions watch_options;
        watch_options.scan_options=options.scan;
        watch_options.interval_ms=options.interval_ms; watch_options.max_updates=options.updates;
        watch_options.force_polling=options.polling;
        // watch 库只在事务成功提交后通知更新；每个结果输出一行并立即刷新流。
        watch_options.on_update=[&options](const mini::ScanResult& result) {
            mini::cli::scan(result,options.json,true,options.profile); std::cout.flush();
        };
        const auto result=mini::watch(database,options.id,watch_options);
        std::cerr << "Watch: updates=" << result.updates << " mode=" << (result.native_events ? "native" : "polling") << '\n';
    } else {
        auto root=mini::detail::from_utf8(options.argument);
        if (options.command==Command::refresh) {
            root=registered_root(database,options.id);
            options.scan.incremental=true; options.scan.expected_root_id=options.id;
        }
        const auto result=mini::scan(root,database,options.scan);
        mini::cli::scan(result,options.json,options.scan.incremental,options.profile);
    }
}
/// 校验参数后调度服务；默认搜索保持原五列 TSV，所有诊断输出到 stderr。
int run(const std::vector<std::string>& args) {
    if (args.size()==1 && (args[0]=="--help" || args[0]=="--version")) { help(); return 0; }
    auto options=parse(args);
    const auto database=mini::detail::from_utf8(options.database);
    const auto start=std::chrono::steady_clock::now();
    int status=0;
    if (maintenance(options.command)) run_maintenance(options,database);
    else switch (options.command) {
    case Command::search: {
        const auto result=mini::query(database,options.query);
        mini::cli::nodes(result,options.json,options.show_ids);
        std::cerr << "Returned " << result.size() << " result(s); limit " << options.query.limit << ".\n";
        break;
    }
    case Command::roots: mini::cli::roots(mini::list_roots(database),options.json); break;
    case Command::stats: {
        const auto result=mini::index_stats(database);
        mini::cli::stats(result.files,result.directories,result.bytes,options.json,result.roots); break;
    }
    case Command::check: {
        const auto result=mini::check_index(database); mini::cli::health(result,options.json); status=result.ok ? 0 : 1; break;
    }
    case Command::forget:
        mini::forget_root(database,options.id);
        if (options.json) std::cout << "{\"forgotten_root\":" << options.id << "}\n";
        else std::cout << "Forgot root " << options.id << ".\n";
        break;
    case Command::info: {
        const auto result=mini::get_node(database,options.id);
        if (options.json) { mini::cli::node_json(result); std::cout << '\n'; }
        else { mini::cli::node_header(true); mini::cli::node_row(result,true); }
        break;
    }
    case Command::children: mini::cli::nodes(mini::children(database,options.id,options.query.limit,options.query.offset),options.json); break;
    case Command::ancestors: mini::cli::nodes(mini::ancestors(database,options.id),options.json); break;
    case Command::tree: mini::cli::tree(mini::tree(database,options.id,options.depth,options.tree_limit),options.json); break;
    case Command::du: {
        const auto result=mini::summarize(database,options.id);
        mini::cli::stats(result.files,result.directories,result.bytes,options.json); break;
    }
    case Command::scan: case Command::refresh: case Command::watch: break;
    }
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    std::cerr << "Elapsed: " << elapsed << " ms\n";
    return status;
}
} // namespace

/// 将 Windows 宽字符参数转换为 UTF-8，映射取消、参数错误和操作故障到约定退出码。
int main(int argc, char** argv) {
    try {
        std::vector<std::string> args;
#ifdef _WIN32
        (void)argc; (void)argv;
        SetConsoleOutputCP(CP_UTF8);
        int count=0;
        auto* wide=CommandLineToArgvW(GetCommandLineW(),&count);
        if (!wide) throw std::runtime_error("Cannot read command line");
        try { for (int i=1;i<count;++i) args.push_back(mini::detail::utf8(std::filesystem::path(wide[i]))); }
        catch (...) { LocalFree(wide); throw; }
        LocalFree(wide);
#else
        for (int i=1;i<argc;++i) args.emplace_back(argv[i]);
#endif
        return run(args);
    } catch (const mini::ScanCancelled&) {
        std::cerr << "Cancelled: current operation stopped; last committed index retained.\n"; return 130;
    } catch (const std::invalid_argument& e) {
        std::cerr << "Usage error: " << mini::cli::escaped(e.what()) << '\n'; return 2;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << mini::cli::escaped(e.what()) << '\n'; return 1;
    }
}
