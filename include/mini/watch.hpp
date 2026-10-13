#pragma once
#include "index.hpp"

namespace mini {
struct WatchOptions {
    ScanOptions scan_options;
    int interval_ms{1000}; // minimum time between reconciliations / polling interval
    int max_updates{0}; // 0 keeps running; initial reconciliation counts as an update
    bool force_polling{false};
    std::function<void(const ScanResult&)> on_update;
};
struct WatchResult {
    std::int64_t updates{};
    bool native_events{};
};
/// 维护一个已注册根：先捕获变化，再增量校准；Windows 使用事件，其他情况按间隔轮询。
/// 每轮单独提交，取消当前轮会回滚；on_update 在成功提交后调用，不能回滚已完成的轮次。
WatchResult watch(const std::filesystem::path& database, std::int64_t root_id, const WatchOptions& options = {});
} // namespace mini
