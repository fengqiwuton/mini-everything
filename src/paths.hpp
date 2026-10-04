#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#endif

namespace mini::detail {
namespace fs = std::filesystem;
inline std::string utf8(const fs::path& path) {
    const auto s = path.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}
inline fs::path from_utf8(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}
inline fs::path normalized(const fs::path& path) {
    if (path.empty()) throw std::invalid_argument("Path must not be empty");
#ifdef _WIN32
    auto input = path.wstring();
    std::replace(input.begin(), input.end(), L'/', L'\\');
    // GetFullPathName handles relative paths without MinGW's extended-path bug.
    const DWORD needed = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (!needed) throw std::system_error(GetLastError(), std::system_category(), "Resolve absolute path");
    std::wstring absolute(needed, L'\0');
    const DWORD copied = GetFullPathNameW(input.c_str(), needed, absolute.data(), nullptr);
    if (!copied || copied >= needed) throw std::runtime_error("Cannot resolve absolute path");
    absolute.resize(copied);
    if (absolute.rfind(L"\\\\?\\", 0) != 0) {
        if (absolute.rfind(L"\\\\", 0) == 0) absolute = L"\\\\?\\UNC\\" + absolute.substr(2);
        else absolute = L"\\\\?\\" + absolute;
    }
    std::size_t root_length = 7; // Extended drive root, including its separator.
    if (absolute.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        const auto server = absolute.find(L'\\', 8);
        const auto share = server == std::wstring::npos ? server : absolute.find(L'\\', server + 1);
        if (server == std::wstring::npos) throw std::invalid_argument("UNC path requires a server and share");
        if (share == std::wstring::npos) absolute += L'\\';
        root_length = share == std::wstring::npos ? absolute.size() : share + 1;
    }
    while (absolute.size() > root_length && absolute.back() == L'\\') absolute.pop_back();
    // Resolve filesystem identity aliases (8.3, junction ancestors, SUBST) through
    // the deepest existing prefix. The database leaf may not exist yet.
    auto existing = absolute;
    std::vector<std::wstring> suffix;
    while (GetFileAttributesW(existing.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            throw std::system_error(error, std::system_category(), "Read path attributes");
        if (existing.size() <= root_length) throw std::runtime_error("Path has no accessible volume");
        const auto separator = existing.find_last_of(L'\\');
        suffix.push_back(existing.substr(separator + 1));
        existing.resize(std::max(separator, root_length));
    }
    std::wstring final_path(32768, L'\0');
    HANDLE handle = CreateFileW(existing.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::system_error(GetLastError(), std::system_category(), "Open path for normalization");
    const DWORD length = GetFinalPathNameByHandleW(handle, final_path.data(),
        static_cast<DWORD>(final_path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);
    if (length == 0 || length >= final_path.size()) throw std::runtime_error("Cannot resolve final path");
    final_path.resize(length);
    for (auto it = suffix.rbegin(); it != suffix.rend(); ++it) {
        if (final_path.back() != L'\\') final_path += L'\\';
        final_path += *it;
    }
    return fs::path(final_path);
#else
    auto result = fs::weakly_canonical(fs::absolute(path));
    while (result != result.root_path() && result.filename().empty()) result = result.parent_path();
    return result;
#endif
}
inline bool component_equal(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return a == b;
#endif
}
// Component comparison prevents C:\root from matching C:\root-other.
inline bool contains(const fs::path& parent, const fs::path& child) {
    auto p = parent.begin(); auto c = child.begin();
    for (; p != parent.end(); ++p) {
        // A drive/volume root can end with an empty iterator component.
        if (p->empty()) continue;
        if (c == child.end() || !component_equal(*p, *c)) return false;
        ++c;
    }
    return true;
}
inline bool is_link(const fs::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        throw std::runtime_error("Cannot read attributes: " + utf8(path));
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return fs::is_symlink(fs::symlink_status(path));
#endif
}
inline std::string ascii_lower(std::string text) {
    for (char& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return text;
}
} // namespace mini::detail
