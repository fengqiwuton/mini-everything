#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mini {
struct Node {
    std::int64_t id{};
    std::int64_t parent_id{}; // 0 means index root
    std::string name;
    std::string path;
    bool is_directory{};
    std::int64_t size{};
    std::int64_t modified_at{}; // Unix seconds
};

struct ScanTimings {
    double prepare_ms{}; // database open, schema and previous-root cleanup
    double metadata_ms{}; // enumeration and metadata
    double identity_ms{};
    double write_ms{}; // SQL and serialization
    double commit_ms{};
};
struct ScanResult {
    std::int64_t files{};
    std::int64_t directories{}; // includes root
    std::int64_t skipped{}; // links / unsupported file types
    std::int64_t identity_checks{}; // per-file identity comparisons
    ScanTimings timings;
};

struct SearchOptions {
    std::string text;
    std::string extension{}; // optional; leading dot accepted
    int limit{100}; // 1..10000
};

// Paths at the public boundary are native filesystem paths; stored text is UTF-8.
ScanResult scan(const std::filesystem::path& root, const std::filesystem::path& database);
std::vector<Node> search(const std::filesystem::path& database, const SearchOptions& options);
} // namespace mini
