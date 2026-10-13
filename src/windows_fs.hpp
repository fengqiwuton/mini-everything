#pragma once
#ifdef _WIN32
#include "paths.hpp"
#include <cstdint>

namespace mini::detail {
class FileHandle {
    HANDLE handle_{INVALID_HANDLE_VALUE};
public:
    /// 用共享读写删除模式打开路径以查询文件身份，支持目录；打开失败时抛出系统异常。
    explicit FileHandle(const fs::path& path) {
        handle_ = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            throw std::system_error(GetLastError(), std::system_category(), "Open file identity: " + utf8(path));
    }
    /// 关闭本对象持有的 Windows 文件句柄。
    ~FileHandle() { CloseHandle(handle_); }
    /// 禁止复制句柄所有权，避免重复关闭同一文件句柄。
    FileHandle(const FileHandle&) = delete;
    /// 禁止赋值共享文件句柄，保持独占的资源管理责任。
    FileHandle& operator=(const FileHandle&) = delete;
    /// 读取卷序列号、文件 ID、硬链接数等身份信息，系统调用失败时抛出异常。
    BY_HANDLE_FILE_INFORMATION info() const {
        BY_HANDLE_FILE_INFORMATION value{};
        if (!GetFileInformationByHandle(handle_, &value))
            throw std::system_error(GetLastError(), std::system_category(), "Read file identity");
        return value;
    }
};

/// 判断规范扩展路径是否位于明确的本地盘类型上，为单链接数据库快速路径提供依据。
inline bool is_local_drive_path(const fs::path& path) {
    const auto native = path.wstring();
    if (native.size() < 7 || native.compare(0, 4, L"\\\\?\\") != 0 ||
        native[5] != L':' || native[6] != L'\\') return false;
    const auto drive = native.substr(4, 3);
    const auto type = GetDriveTypeW(drive.c_str());
    return type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_RAMDISK || type == DRIVE_CDROM;
}

class IndexFileGuard {
    FileHandle handle_;
    BY_HANDLE_FILE_INFORMATION initial_;
    bool allow_fast_path_;
public:
    /// 持续持有数据库句柄并缓存初始身份及链接数，调用方显式决定是否允许快速路径。
    explicit IndexFileGuard(const fs::path& path, bool allow_single_link_fast_path = false)
        : handle_(path), initial_(handle_.info()), allow_fast_path_(allow_single_link_fast_path) {}
    // UNC and mapped-drive aliases can refer to the same single-link file.
    /// 判断是否需要逐文件身份检查；快速路径未获允许或初始链接数不为 1 时返回 true。
    bool needs_checks() const { return !allow_fast_path_ || initial_.nNumberOfLinks != 1; }
    /// 比较候选路径的卷序列号和文件 ID，判断它是否指向同一个索引数据库文件。
    bool matches(const fs::path& path) const {
        const auto candidate = FileHandle(path).info();
        return initial_.dwVolumeSerialNumber == candidate.dwVolumeSerialNumber &&
            initial_.nFileIndexHigh == candidate.nFileIndexHigh && initial_.nFileIndexLow == candidate.nFileIndexLow;
    }
    /// 提交前重读数据库链接数；若相对初始值发生变化则抛错，促使扫描事务回滚。
    void verify_unchanged() const {
        if (handle_.info().nNumberOfLinks != initial_.nNumberOfLinks)
            throw std::runtime_error("Index hard-link count changed during scan; retry the scan");
    }
};

/// 将 Windows FILETIME 的 100 纳秒计数转换为以 Unix 纪元为起点的整数秒。
inline std::int64_t unix_seconds(FILETIME value) {
    const auto ticks = (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    return static_cast<std::int64_t>(ticks / 10000000) - 11644473600LL;
}
struct EntryMetadata {
    fs::path path;
    bool directory{};
    bool skipped{};
    std::uint64_t size{};
    std::int64_t modified_at{};
};
/// 一次读取路径属性、大小和修改时间，标记重解析点；读取失败时抛出系统异常。
inline EntryMetadata read_metadata(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        throw std::system_error(GetLastError(), std::system_category(), "Read file metadata: " + utf8(path));
    return {path, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
        (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0,
        (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow, unix_seconds(data.ftLastWriteTime)};
}

class DirectoryCursor {
    HANDLE handle_{INVALID_HANDLE_VALUE};
    WIN32_FIND_DATAW data_{};
    fs::path directory_;
    bool first_{true};
public:
    /// 为指定目录建立原生枚举游标；空目录允许直接结束，其他枚举失败抛出异常。
    explicit DirectoryCursor(const fs::path& directory) : directory_(directory) {
        auto pattern = directory.wstring();
        if (pattern.back() != L'\\') pattern += L'\\';
        pattern += L'*';
        handle_ = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data_, FindExSearchNameMatch, nullptr, 0);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND && read_metadata(directory).directory) return;
            throw std::system_error(error, std::system_category(), "Enumerate directory: " + utf8(directory));
        }
    }
    /// 释放有效的 Windows 目录枚举句柄。
    ~DirectoryCursor() { if (handle_ != INVALID_HANDLE_VALUE) FindClose(handle_); }
    /// 禁止复制游标，避免共享枚举进度和重复释放句柄。
    DirectoryCursor(const DirectoryCursor&) = delete;
    /// 禁止赋值覆盖游标所有权，保持枚举状态与原目录绑定。
    DirectoryCursor& operator=(const DirectoryCursor&) = delete;
    /// 返回下一项路径及元数据，略过点目录并标记待跳过的重解析点/设备。
    /// 到达结尾返回 false，枚举中途的读取失败抛出异常。
    bool next(EntryMetadata& entry) {
        if (handle_ == INVALID_HANDLE_VALUE) return false;
        for (;;) {
            if (first_) first_ = false;
            else if (!FindNextFileW(handle_, &data_)) {
                const DWORD error = GetLastError();
                if (error != ERROR_NO_MORE_FILES)
                    throw std::system_error(error, std::system_category(), "Continue directory enumeration");
                return false;
            }
            const std::wstring name = data_.cFileName;
            if (name == L"." || name == L"..") continue;
            auto path = directory_.wstring();
            if (path.back() != L'\\') path += L'\\';
            path += name;
            entry = {fs::path(path), (data_.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                (data_.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) != 0,
                (static_cast<std::uint64_t>(data_.nFileSizeHigh) << 32) | data_.nFileSizeLow,
                unix_seconds(data_.ftLastWriteTime)};
            return true;
        }
    }
};
} // namespace mini::detail
#endif
