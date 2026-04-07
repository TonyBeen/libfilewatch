#ifndef FILEWATCH_PATHUTILS_H
#define FILEWATCH_PATHUTILS_H

#include <string>

namespace filewatch {
namespace utils {

/**
 * 路径工具类
 */
class PathUtils {
public:
    /**
     * 规范化路径
     * @param path 原始路径
     * @return 规范化后的路径
     */
    static std::string normalize(const std::string& path);

    /**
     * 获取绝对路径
     * @param path 相对路径
     * @return 绝对路径
     */
    static std::string getAbsolutePath(const std::string& path);

    /**
     * 检查路径是否存在
     * @param path 路径
     * @return 是否存在
     */
    static bool exists(const std::string& path);

    /**
     * 检查路径是否为目录
     * @param path 路径
     * @return 是否为目录
     */
    static bool isDirectory(const std::string& path);

    /**
     * 检查路径是否为文件
     * @param path 路径
     * @return 是否为文件
     */
    static bool isFile(const std::string& path);
};

} // namespace utils
} // namespace filewatch

#endif // FILEWATCH_PATHUTILS_H