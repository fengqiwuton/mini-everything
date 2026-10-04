#include "mini/index.hpp"
#include "paths.hpp"
#include <charconv>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <set>
#ifdef _WIN32
#include <shellapi.h>
#endif

namespace {
void help() {
    std::cout << "MiniEverything 0.2.0\n"
        "  mini-everything scan <directory> --db <index.db> [--profile]\n"
        "  mini-everything search <text> --db <index.db> [--ext pdf] [--limit 100]\n"
        "  mini-everything --help | --version\n"
        "Search is a literal substring match (ASCII case-insensitive).\n"
        "Database must be outside the scanned directory. Links are skipped.\n"
        "Exit codes: 0 success, 1 operation failed, 2 invalid arguments.\n";
}
std::string escaped(const std::string& text) {
    std::string out;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c < 32 || c == 127) { out += "\\x"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out;
}
int run(const std::vector<std::string>& args) {
    if (args.size() == 1 && (args[0] == "--help" || args[0] == "--version")) { help(); return 0; }
    if (args.size() < 2 || (args[0] != "scan" && args[0] != "search"))
        throw std::invalid_argument("Expected scan or search; use --help");
    std::string database;
    mini::SearchOptions options;
    options.text = args[1];
    std::set<std::string> seen;
    bool profile = false;
    for (std::size_t i = 2; i < args.size();) {
        const auto& flag = args[i];
        if (!seen.insert(flag).second)
            throw std::invalid_argument("Missing value or duplicate option: " + flag);
        if (flag == "--profile" && args[0] == "scan") { profile = true; ++i; continue; }
        if (i + 1 == args.size()) throw std::invalid_argument("Missing value: " + flag);
        const auto& value = args[i + 1];
        if (flag == "--db") database = value;
        else if (args[0] == "search" && flag == "--ext") options.extension = value;
        else if (args[0] == "search" && flag == "--limit") {
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), options.limit);
            if (error != std::errc{} || end != value.data() + value.size() || options.limit < 1 || options.limit > 10000)
                throw std::invalid_argument("--limit must be an integer between 1 and 10000");
        } else throw std::invalid_argument("Unknown option: " + flag);
        i += 2;
    }
    if (database.empty()) throw std::invalid_argument("--db is required");
    const auto db_path = mini::detail::from_utf8(database);
    const auto start = std::chrono::steady_clock::now();
    if (args[0] == "scan") {
        const auto result = mini::scan(mini::detail::from_utf8(args[1]), db_path);
        std::cout << "Indexed " << result.files << " files, " << result.directories
            << " directories; skipped " << result.skipped << " links/unsupported entries.\n";
        if (profile) std::cerr << std::fixed << std::setprecision(2)
            << "Profile: prepare_ms=" << result.timings.prepare_ms << " metadata_ms=" << result.timings.metadata_ms
            << " identity_ms=" << result.timings.identity_ms << " write_ms=" << result.timings.write_ms
            << " commit_ms=" << result.timings.commit_ms << " identity_checks=" << result.identity_checks << '\n';
    } else {
        const auto results = mini::search(db_path, options);
        std::cout << "TYPE\tSIZE\tMODIFIED_UNIX\tNAME\tPATH\n";
        for (const auto& item : results)
            std::cout << (item.is_directory ? "dir" : "file") << '\t' << item.size << '\t'
                << item.modified_at << '\t' << escaped(item.name) << '\t' << escaped(item.path) << '\n';
        std::cerr << "Returned " << results.size() << " result(s); limit " << options.limit << ".\n";
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    std::cerr << "Elapsed: " << ms << " ms\n";
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::vector<std::string> args;
#ifdef _WIN32
        (void)argc; (void)argv;
        SetConsoleOutputCP(CP_UTF8);
        int count = 0;
        auto* wide = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!wide) throw std::runtime_error("Cannot read command line");
        try {
            for (int i = 1; i < count; ++i) args.push_back(mini::detail::utf8(std::filesystem::path(wide[i])));
        } catch (...) { LocalFree(wide); throw; }
        LocalFree(wide);
#else
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
#endif
        return run(args);
    } catch (const std::invalid_argument& e) {
        std::cerr << "Usage error: " << escaped(e.what()) << '\n'; return 2;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << escaped(e.what()) << '\n'; return 1;
    }
}
