#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
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
    auto result = fs::weakly_canonical(fs::absolute(path));
    while (result != result.root_path() && result.filename().empty()) result = result.parent_path();
#ifdef _WIN32
    // Resolve filesystem identity aliases (8.3, junction ancestors, SUBST) through
    // the deepest existing prefix. The database leaf may not exist yet.
    auto existing = result;
    std::vector<fs::path> suffix;
    while (!fs::exists(existing)) {
        if (existing == existing.root_path()) throw std::runtime_error("Path has no accessible volume");
        suffix.push_back(existing.filename());
        existing = existing.parent_path();
    }
    std::wstring final_path(32768, L'\0');
    HANDLE handle = CreateFileW(existing.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot resolve path: " + utf8(existing));
    const DWORD length = GetFinalPathNameByHandleW(handle, final_path.data(),
        static_cast<DWORD>(final_path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);
    if (length == 0 || length >= final_path.size()) throw std::runtime_error("Cannot resolve final path: " + utf8(existing));
    final_path.resize(length);
    result = fs::path(final_path); // Keep \\?\ for long-path support.
    for (auto it = suffix.rbegin(); it != suffix.rend(); ++it) result /= *it;
#endif
    return result;
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
    for (; p != parent.end(); ++p, ++c)
        if (c == child.end() || !component_equal(*p, *c)) return false;
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
