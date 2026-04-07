#ifndef FILEWATCH_ENCODINGUTILS_H
#define FILEWATCH_ENCODINGUTILS_H

#include <string>

namespace filewatch {
namespace utils {

/**
 * 编码转换工具类
 * 主要用于 Windows 平台上的 UTF-8 和 Unicode 转换
 */
class EncodingUtils {
public:
    /**
     * UTF-8 转 UTF-16 (Windows Unicode)
     * @param utf8 UTF-8 字符串
     * @return UTF-16 字符串
     */
    static std::wstring utf8ToUtf16(const std::string& utf8);

    /**
     * UTF-16 (Windows Unicode) 转 UTF-8
     * @param utf16 UTF-16 字符串
     * @return UTF-8 字符串
     */
    static std::string utf16ToUtf8(const std::wstring& utf16);

    /**
     * 确保路径在 Windows 上使用正确的编码
     * @param path UTF-8 路径
     * @return 处理后的 UTF-8 路径
     */
    static std::string ensureWindowsPathEncoding(const std::string& path);
};

} // namespace utils
} // namespace filewatch

#endif // FILEWATCH_ENCODINGUTILS_H