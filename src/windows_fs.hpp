#pragma once
#ifdef _WIN32
#include "paths.hpp"
#include <cstdint>

namespace mini::detail {
class FileHandle {
    HANDLE handle_{INVALID_HANDLE_VALUE};
public:
    explicit FileHandle(const fs::path& path) {
        handle_ = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            throw std::system_error(GetLastError(), std::system_category(), "Open file identity: " + utf8(path));
    }
    ~FileHandle() { CloseHandle(handle_); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    BY_HANDLE_FILE_INFORMATION info() const {
        BY_HANDLE_FILE_INFORMATION value{};
        if (!GetFileInformationByHandle(handle_, &value))
            throw std::system_error(GetLastError(), std::system_category(), "Read file identity");
        return value;
    }
};

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
    explicit IndexFileGuard(const fs::path& path, bool allow_single_link_fast_path = false)
        : handle_(path), initial_(handle_.info()), allow_fast_path_(allow_single_link_fast_path) {}
    // UNC and mapped-drive aliases can refer to the same single-link file.
    bool needs_checks() const { return !allow_fast_path_ || initial_.nNumberOfLinks != 1; }
    bool matches(const fs::path& path) const {
        const auto candidate = FileHandle(path).info();
        return initial_.dwVolumeSerialNumber == candidate.dwVolumeSerialNumber &&
            initial_.nFileIndexHigh == candidate.nFileIndexHigh && initial_.nFileIndexLow == candidate.nFileIndexLow;
    }
    void verify_unchanged() const {
        if (handle_.info().nNumberOfLinks != initial_.nNumberOfLinks)
            throw std::runtime_error("Index hard-link count changed during scan; retry the scan");
    }
};

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
    ~DirectoryCursor() { if (handle_ != INVALID_HANDLE_VALUE) FindClose(handle_); }
    DirectoryCursor(const DirectoryCursor&) = delete;
    DirectoryCursor& operator=(const DirectoryCursor&) = delete;
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
