#include "utils/EncodingUtils.h"

#ifdef _WIN32
    #include <windows.h>
#endif

namespace filewatch {
namespace utils {

std::wstring EncodingUtils::utf8ToUtf16(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) {
        return std::wstring();
    }

    int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (length == 0) {
        return std::wstring();
    }

    std::wstring utf16(length - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &utf16[0], length);
    return utf16;
#else
    // 在非 Windows 平台，直接返回空字符串
    return std::wstring();
#endif
}

std::string EncodingUtils::utf16ToUtf8(const std::wstring& utf16) {
#ifdef _WIN32
    if (utf16.empty()) {
        return std::string();
    }

    int length = WideCharToMultiByte(CP_UTF8, 0, utf16.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length == 0) {
        return std::string();
    }

    std::string utf8(length - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.c_str(), -1, &utf8[0], length, nullptr, nullptr);
    return utf8;
#else
    // 在非 Windows 平台，直接返回空字符串
    return std::string();
#endif
}

std::string EncodingUtils::ensureWindowsPathEncoding(const std::string& path) {
#ifdef _WIN32
    // 转换为 UTF-16 再转换回 UTF-8，确保编码正确
    std::wstring utf16Path = utf8ToUtf16(path);
    return utf16ToUtf8(utf16Path);
#else
    // 在非 Windows 平台，直接返回原路径
    return path;
#endif
}

} // namespace utils
} // namespace filewatch