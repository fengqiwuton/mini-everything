#pragma once
#include "mini/catalog.hpp"
#include "mini/query.hpp"
#include <iomanip>
#include <iostream>
#include <optional>

namespace mini::cli {
/// 把控制字节编码为可见十六进制文本，保持旧 TSV 的单行输出约定。
inline std::string escaped(const std::string& text) {
    std::string out;
    constexpr char hex[]="0123456789abcdef";
    for (unsigned char c : text) {
        if (c<32 || c==127) { out+="\\x"; out+=hex[c>>4]; out+=hex[c&15]; }
        else out+=static_cast<char>(c);
    }
    return out;
}
/// 按 JSON 规则引用文本，转义引号、反斜杠和所有控制字节，保留 UTF-8 非 ASCII 字节。
inline std::string json_string(const std::string& text) {
    std::string out="\"";
    constexpr char hex[]="0123456789abcdef";
    for (unsigned char c : text) {
        if (c=='"' || c=='\\') { out+='\\'; out+=static_cast<char>(c); }
        else if (c<32) { out+="\\u00"; out+=hex[c>>4]; out+=hex[c&15]; }
        else out+=static_cast<char>(c);
    }
    return out+'"';
}
/// 输出一个完整节点 JSON 对象；树结果额外带有同层 depth 字段。
inline void node_json(const Node& node, std::optional<int> depth={}) {
    std::cout << "{\"id\":" << node.id << ",\"parent_id\":" << node.parent_id
        << ",\"name\":" << json_string(node.name) << ",\"path\":" << json_string(node.path)
        << ",\"is_directory\":" << (node.is_directory ? "true" : "false")
        << ",\"size\":" << node.size << ",\"modified_at\":" << node.modified_at;
    if (depth) std::cout << ",\"depth\":" << *depth;
    std::cout << '}';
}
/// 输出节点 TSV 标题，默认搜索保留原有五列，显式请求或目录命令可带 ID。
inline void node_header(bool ids) {
    if (ids) std::cout << "ID\tPARENT_ID\t";
    std::cout << "TYPE\tSIZE\tMODIFIED_UNIX\tNAME\tPATH\n";
}
/// 输出单个节点 TSV 行，并对名称和路径应用旧的控制字节转义。
inline void node_row(const Node& node, bool ids) {
    if (ids) std::cout << node.id << '\t' << node.parent_id << '\t';
    std::cout << (node.is_directory ? "dir" : "file") << '\t' << node.size << '\t'
        << node.modified_at << '\t' << escaped(node.name) << '\t' << escaped(node.path) << '\n';
}
/// 输出节点数组或 TSV 表，用于搜索、子项和祖先查询。
inline void nodes(const std::vector<Node>& values, bool json, bool ids=true) {
    if (json) {
        std::cout << '[';
        bool first=true;
        for (const auto& node : values) { if (!first) std::cout << ','; first=false; node_json(node); }
        std::cout << "]\n";
    } else {
        node_header(ids);
        for (const auto& node : values) node_row(node,ids);
    }
}
/// 输出含深度的树，JSON 节点字段保持扁平化，TSV 在节点列之前显示深度。
inline void tree(const std::vector<TreeEntry>& values, bool json) {
    if (json) {
        std::cout << '[';
        bool first=true;
        for (const auto& entry : values) { if (!first) std::cout << ','; first=false; node_json(entry.node,entry.depth); }
        std::cout << "]\n";
    } else {
        std::cout << "DEPTH\t"; node_header(true);
        for (const auto& entry : values) { std::cout << entry.depth << '\t'; node_row(entry.node,true); }
    }
}
/// 输出注册根列表，分别提供根 ID、根节点 ID、数量和扫描时间。
inline void roots(const std::vector<CatalogRoot>& values, bool json) {
    if (json) {
        std::cout << '[';
        bool first=true;
        for (const auto& root : values) {
            if (!first) std::cout << ',';
            first=false;
            std::cout << "{\"id\":" << root.id << ",\"path\":" << json_string(root.path)
                << ",\"scanned_at\":" << root.scanned_at << ",\"node_id\":" << root.node_id
                << ",\"files\":" << root.files << ",\"directories\":" << root.directories
                << ",\"bytes\":" << root.bytes << '}';
        }
        std::cout << "]\n";
    } else {
        std::cout << "ROOT_ID\tNODE_ID\tFILES\tDIRECTORIES\tBYTES\tSCANNED_UNIX\tPATH\n";
        for (const auto& root : values) std::cout << root.id << '\t' << root.node_id << '\t'
            << root.files << '\t' << root.directories << '\t' << root.bytes << '\t'
            << root.scanned_at << '\t' << escaped(root.path) << '\n';
    }
}
/// 输出全库或子树的数量统计，全库统计额外提供注册根数量。
inline void stats(std::int64_t files, std::int64_t directories, std::int64_t bytes,
                  bool json, std::optional<std::int64_t> root_count={}) {
    if (json) {
        std::cout << '{';
        if (root_count) std::cout << "\"roots\":" << *root_count << ',';
        std::cout << "\"files\":" << files << ",\"directories\":" << directories << ",\"bytes\":" << bytes << "}\n";
    } else {
        if (root_count) std::cout << "ROOTS\t";
        std::cout << "FILES\tDIRECTORIES\tBYTES\n";
        if (root_count) std::cout << *root_count << '\t';
        std::cout << files << '\t' << directories << '\t' << bytes << '\n';
    }
}
/// 输出健康状态及诊断类别，诊断文本在 JSON 和 TSV 中都保持有效编码。
inline void health(const IndexHealth& result, bool json) {
    if (json) {
        std::cout << "{\"ok\":" << (result.ok ? "true" : "false") << ",\"issues\":[";
        bool first=true;
        for (const auto& issue : result.issues) { if (!first) std::cout << ','; first=false; std::cout << json_string(issue); }
        std::cout << "]}\n";
    } else {
        std::cout << "OK\tISSUE\n";
        if (result.issues.empty()) std::cout << (result.ok ? "true" : "false") << "\t\n";
        else for (const auto& issue : result.issues) std::cout << "false\t" << escaped(issue) << '\n';
    }
}
/// 保留原有扫描概要和全部性能诊断字段，JSON 扫描结果独占 stdout 的一行。
inline void scan(const ScanResult& result, bool json, bool incremental, bool profile) {
    if (json) std::cout << "{\"files\":" << result.files << ",\"directories\":" << result.directories
        << ",\"skipped\":" << result.skipped << ",\"inserted\":" << result.inserted
        << ",\"updated\":" << result.updated << ",\"removed\":" << result.removed
        << ",\"unchanged\":" << result.unchanged << "}\n";
    else std::cout << "Indexed " << result.files << " files, " << result.directories
        << " directories; skipped " << result.skipped << " links/unsupported entries.\n";
    if (incremental) std::cerr << "Changes: inserted=" << result.inserted << " updated=" << result.updated
        << " removed=" << result.removed << " unchanged=" << result.unchanged << '\n';
    if (profile) std::cerr << std::fixed << std::setprecision(2)
        << "Profile: prepare_ms=" << result.timings.prepare_ms
        << " open_ms=" << result.timings.open_ms << " transaction_ms=" << result.timings.transaction_ms
        << " schema_ms=" << result.timings.schema_ms << " roots_ms=" << result.timings.roots_ms
        << " clear_ms=" << result.timings.clear_ms << " statements_ms=" << result.timings.statements_ms
        << " metadata_ms=" << result.timings.metadata_ms << " identity_ms=" << result.timings.identity_ms
        << " write_ms=" << result.timings.write_ms << " commit_ms=" << result.timings.commit_ms
        << " identity_checks=" << result.identity_checks << " db_cache_mib=" << result.database_stats.cache_mib
        << " cache_hits=" << result.database_stats.cache_hits << " cache_misses=" << result.database_stats.cache_misses
        << " cache_writes=" << result.database_stats.cache_writes << " cache_spills=" << result.database_stats.cache_spills << '\n';
}
} // namespace mini::cli
