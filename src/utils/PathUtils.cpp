#include "utils/PathUtils.h"
#include "utils/EncodingUtils.h"

#ifdef _WIN32
    #include <windows.h>
#elif __linux__ || __APPLE__
    #include <sys/stat.h>
    #include <unistd.h>
    #include <limits.h>
#endif

namespace filewatch {
namespace utils {

std::string PathUtils::normalize(const std::string& path) {
#ifdef _WIN32
    std::string result = path;
    // 将正斜杠替换为反斜杠
    for (size_t i = 0; i < result.size(); ++i) {
        if (result[i] == '/') {
            result[i] = '\\';
        }
    }
    return result;
#else
    return path;
#endif
}

std::string PathUtils::getAbsolutePath(const std::string& path) {
#ifdef _WIN32
    std::wstring widePath = EncodingUtils::utf8ToUtf16(path);
    if (widePath.empty()) {
        return path;
    }

    wchar_t buffer[MAX_PATH];
    if (GetFullPathNameW(widePath.c_str(), MAX_PATH, buffer, nullptr) != 0) {
        return EncodingUtils::utf16ToUtf8(std::wstring(buffer));
    }
    return path;
#else
    char buffer[PATH_MAX];
    if (realpath(path.c_str(), buffer) != nullptr) {
        return std::string(buffer);
    }
    return path;
#endif
}

bool PathUtils::exists(const std::string& path) {
#ifdef _WIN32
    std::wstring widePath = EncodingUtils::utf8ToUtf16(path);
    if (widePath.empty()) {
        return false;
    }
    DWORD attributes = GetFileAttributesW(widePath.c_str());
    return (attributes != INVALID_FILE_ATTRIBUTES);
#else
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
#endif
}

bool PathUtils::isDirectory(const std::string& path) {
#ifdef _WIN32
    std::wstring widePath = EncodingUtils::utf8ToUtf16(path);
    if (widePath.empty()) {
        return false;
    }
    DWORD attributes = GetFileAttributesW(widePath.c_str());
    return (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY));
#else
    struct stat buffer;
    if (stat(path.c_str(), &buffer) == 0) {
        return S_ISDIR(buffer.st_mode);
    }
    return false;
#endif
}

bool PathUtils::isFile(const std::string& path) {
#ifdef _WIN32
    std::wstring widePath = EncodingUtils::utf8ToUtf16(path);
    if (widePath.empty()) {
        return false;
    }
    DWORD attributes = GetFileAttributesW(widePath.c_str());
    return (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY));
#else
    struct stat buffer;
    if (stat(path.c_str(), &buffer) == 0) {
        return S_ISREG(buffer.st_mode);
    }
    return false;
#endif
}

} // namespace utils
} // namespace filewatch