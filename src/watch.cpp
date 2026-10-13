#include "mini/watch.hpp"
#include "read_index.hpp"
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <thread>
#ifdef _WIN32
#include "windows_fs.hpp"
#else
#include <sys/stat.h>
#endif

namespace mini {
namespace {
using namespace detail;
using Clock = std::chrono::steady_clock;

/// 只读取指定根的登记路径，避免监听空闲期间为验证注册状态而反复聚合全部节点。
fs::path registered_root(const fs::path& database, std::int64_t id) {
    ReadIndex index(database);
    Statement root(index.db,"SELECT path FROM roots WHERE id=?");
    root.bind(1,id);
    if (!root.row()) throw std::invalid_argument("Index root no longer exists");
    return from_utf8(root.text(0));
}

/// 检查调用者的停止请求，统一用 ScanCancelled 表达当前操作取消。
void check_cancel(const WatchOptions& options) {
    if (options.scan_options.cancelled && options.scan_options.cancelled()) throw ScanCancelled();
}

class RootIdentity {
#ifdef _WIN32
    FileHandle handle_;
    BY_HANDLE_FILE_INFORMATION initial_;
#else
    struct stat initial_{};
#endif
public:
    /// 记录物理根目录身份，避免根改名后监听继续跟踪旧句柄或同路径的新目录。
    explicit RootIdentity(const fs::path& root)
#ifdef _WIN32
        : handle_(root), initial_(handle_.info())
#endif
    {
#ifndef _WIN32
        if(::stat(root.c_str(),&initial_)!=0)
            throw std::system_error(errno,std::generic_category(),"Read watched root identity");
#endif
        verify(root);
    }
    /// 周期重开登记路径并核对目录身份；根删除、替换或不可访问时终止维护并保留已提交索引。
    void verify(const fs::path& root) const {
#ifdef _WIN32
        const auto current=FileHandle(root).info();
        const bool same=(current.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0 &&
            current.dwVolumeSerialNumber==initial_.dwVolumeSerialNumber &&
            current.nFileIndexHigh==initial_.nFileIndexHigh && current.nFileIndexLow==initial_.nFileIndexLow;
        if(normalized(root)!=root)
            throw std::runtime_error("Watched root canonical path changed; refresh before restarting watch");
#else
        struct stat current{};
        if(::stat(root.c_str(),&current)!=0)
            throw std::system_error(errno,std::generic_category(),"Read watched root identity");
        const bool same=S_ISDIR(current.st_mode) && current.st_dev==initial_.st_dev && current.st_ino==initial_.st_ino;
#endif
        if(!same) throw std::runtime_error("Watched root directory was replaced");
    }
};

#ifdef _WIN32
class NativeHandle {
    HANDLE value_{INVALID_HANDLE_VALUE};
public:
    /// 接管一个 Windows 句柄，关闭责任由本包装对象持有。
    explicit NativeHandle(HANDLE value) : value_(value) {}
    /// 关闭有效句柄，兼容 CreateEvent 的空句柄与 CreateFile 的无效句柄。
    ~NativeHandle() { if(value_ && value_!=INVALID_HANDLE_VALUE) CloseHandle(value_); }
    /// 禁止复制句柄，防止同一资源被重复关闭。
    NativeHandle(const NativeHandle&) = delete;
    /// 禁止赋值覆盖句柄所有权。
    NativeHandle& operator=(const NativeHandle&) = delete;
    /// 提供原生调用所需的句柄值，不转移所有权。
    HANDLE get() const { return value_; }
};

class ChangeFeed {
    NativeHandle directory_;
    NativeHandle event_;
    std::array<DWORD,16384> buffer_{}; // DWORD alignment; 64 KiB is also valid for network shares
    OVERLAPPED overlapped_{};
    bool pending_{false}; // worker-owned after construction
    bool native_{false};
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<DWORD> error_{0};
    std::jthread worker_;

    /// 发起一次异步子树监听；只能在前一次 I/O 完成后重用缓冲与 OVERLAPPED。
    bool arm() {
        ResetEvent(event_.get());
        overlapped_={}; overlapped_.hEvent=event_.get();
        constexpr DWORD filters=FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|
            FILE_NOTIFY_CHANGE_ATTRIBUTES|FILE_NOTIFY_CHANGE_SIZE|FILE_NOTIFY_CHANGE_LAST_WRITE;
        const auto queued=ReadDirectoryChangesW(directory_.get(),buffer_.data(),
            static_cast<DWORD>(sizeof(buffer_)),TRUE,filters,nullptr,&overlapped_,nullptr);
        const auto error=queued ? ERROR_SUCCESS : GetLastError();
        if(!queued && error!=ERROR_IO_PENDING) {
            error_.store(error);
            return false;
        }
        pending_=true;
        return true;
    }

    /// 收集完成通知并立刻重新监听；事件只标脏，零字节或溢出也会触发完整元数据校准。
    void collect(std::stop_token stop) noexcept {
        while(!stop.stop_requested()) {
            const auto wait=WaitForSingleObject(event_.get(),100);
            if(wait==WAIT_TIMEOUT) continue;
            if(wait!=WAIT_OBJECT_0) { error_.store(GetLastError()); break; }
            DWORD transferred=0;
            const bool completed=GetOverlappedResult(directory_.get(),&overlapped_,&transferred,FALSE)!=0;
            const auto error=completed ? ERROR_SUCCESS : GetLastError();
            if(error==ERROR_IO_INCOMPLETE) continue;
            pending_=false;
            if(stop.stop_requested()) break;
            if(error!=ERROR_SUCCESS && error!=ERROR_NOTIFY_ENUM_DIR) { error_.store(error); break; }
            generation_.fetch_add(1);
            if(!arm()) break;
        }
        if(pending_) {
            CancelIoEx(directory_.get(),nullptr);
            DWORD transferred=0;
            GetOverlappedResult(directory_.get(),&overlapped_,&transferred,TRUE);
            pending_=false;
        }
    }
public:
    /// 在首次校准前建立原生事件捕获；不支持目录通知的文件系统回退到轮询。
    ChangeFeed(const fs::path& root, bool polling)
        : directory_(polling ? INVALID_HANDLE_VALUE : CreateFileW(root.c_str(),FILE_LIST_DIRECTORY,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED,nullptr)),
          event_(polling ? nullptr : CreateEventW(nullptr,TRUE,FALSE,nullptr)) {
        if(polling) return;
        if(directory_.get()==INVALID_HANDLE_VALUE || !event_.get())
            throw std::system_error(GetLastError(),std::system_category(),"Open directory change feed");
        if(!arm()) {
            const auto error=error_.load();
            if(error==ERROR_INVALID_FUNCTION || error==ERROR_NOT_SUPPORTED) { error_.store(0); return; }
            throw std::system_error(error,std::system_category(),"Start directory notifications");
        }
        native_=true;
        try {
            // 工作线程只维护事件状态，不使用 SQLite 或执行调用者的回调。
            worker_=std::jthread([this](std::stop_token stop) { collect(stop); });
        } catch(...) {
            CancelIoEx(directory_.get(),nullptr);
            DWORD transferred=0;
            GetOverlappedResult(directory_.get(),&overlapped_,&transferred,TRUE);
            pending_=false;
            throw;
        }
    }
    /// 请求工作线程停止并等待取消 I/O 完成，再让缓冲与句柄随对象释放。
    ~ChangeFeed() {
        if(worker_.joinable()) {
            worker_.request_stop();
            CancelIoEx(directory_.get(),nullptr);
            worker_.join();
        }
    }
    /// 禁止复制包含后台线程和异步缓冲的事件源。
    ChangeFeed(const ChangeFeed&) = delete;
    /// 禁止赋值转移活动 I/O 的地址及所有权。
    ChangeFeed& operator=(const ChangeFeed&) = delete;
    /// 返回是否成功启用了 Windows 原生事件捕获。
    bool native() const { return native_; }
    /// 读取单调变化代数，允许主线程发现校准期间出现的新变化。
    std::uint64_t generation() const { return generation_.load(); }
    /// 把工作线程的系统错误交给维护线程处理，防止监听失败后静默保持旧索引。
    void check() const {
        if(const auto error=error_.load()) throw std::system_error(error,std::system_category(),"Directory notifications failed");
    }
};
#else
class ChangeFeed {
public:
    /// 非 Windows 平台使用周期校准，无需原生监听资源。
    ChangeFeed(const fs::path&,bool) {}
    /// 明确报告当前事件源属于轮询回退。
    bool native() const { return false; }
    /// 轮询模式不依赖事件代数，固定返回零。
    std::uint64_t generation() const { return 0; }
    /// 轮询的错误由下一次扫描报告，此处没有后台线程错误。
    void check() const {}
};
#endif
} // namespace

/// 将变化通知或轮询转换成串行增量校准，保证活动写事务只存在于维护主线程中。
WatchResult watch(const std::filesystem::path& database, std::int64_t root_id, const WatchOptions& options) {
    if(root_id<=0 || options.interval_ms<10 || options.interval_ms>3600000 ||
        options.max_updates<0 || options.max_updates>1000000)
        throw std::invalid_argument("Invalid watch root, interval or update count");
    check_cancel(options);
    const auto root=normalized(registered_root(database,root_id));
    RootIdentity identity(root);
    ChangeFeed feed(root,options.force_polling);
    auto scan_options=options.scan_options;
    scan_options.incremental=true;
    scan_options.expected_root_id=root_id;
    WatchResult result{0,feed.native()};
    std::uint64_t seen=0;
    auto due=Clock::now();
    auto validation_due=Clock::now();
    for(;;) {
        check_cancel(options);
        feed.check();
        const auto now=Clock::now();
        const bool update=result.updates==0 || (now>=due && (!feed.native() || feed.generation()!=seen));
        // 空闲时每 250ms 检查根自身；每次校准前也检查，避免每 20ms 重开数据库和目录。
        if(now>=validation_due || update) {
            if(!component_equal(registered_root(database,root_id),root))
                throw std::invalid_argument("Registered root path changed during watch");
            identity.verify(root);
            validation_due=now+std::chrono::milliseconds(250);
        }
        if(update) {
            // 保存校准前的代数，校准期间或其后收到的变化会保留到下一轮。
            const auto generation=feed.generation();
            const auto scanned=scan(root,database,scan_options);
            seen=generation;
            ++result.updates;
            if(options.on_update) options.on_update(scanned);
            if(options.max_updates && result.updates>=options.max_updates) return result;
            due=Clock::now()+std::chrono::milliseconds(options.interval_ms);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
} // namespace mini
