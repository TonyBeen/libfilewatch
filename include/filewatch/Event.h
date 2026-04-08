#ifndef FILEWATCH_EVENT_H
#define FILEWATCH_EVENT_H

#include <string>

namespace filewatch {

/**
 * 事件类型枚举
 */
enum class EventType {
    kCreate,    // 文件/目录创建
    kModify,    // 文件修改
    kDelete,    // 文件/目录删除
    kRename     // 文件/目录重命名
};

/**
 * 路径类型枚举
 */
enum class PathType {
    FILE,      // 文件
    DIRECTORY  // 目录
};

/**
 * 文件事件类
 */
class FileEvent {
public:
    /**
     * 构造函数
     * @param type 事件类型
     * @param path 文件路径
     * @param pathType 路径类型（文件/目录）
     * @param oldPath 旧文件路径（用于重命名事件）
     * @param oldPathType 旧路径类型（用于重命名事件）
     */
    FileEvent(EventType type, const std::string& path, PathType pathType, 
              const std::string& oldPath = "", PathType oldPathType = PathType::FILE);

    /**
     * 获取事件类型
     * @return 事件类型
     */
    EventType getType() const;

    /**
     * 获取文件路径
     * @return 文件路径
     */
    std::string getPath() const;

    /**
     * 获取路径类型
     * @return 路径类型
     */
    PathType getPathType() const;

    /**
     * 获取旧文件路径
     * @return 旧文件路径
     */
    std::string getOldPath() const;

    /**
     * 获取旧路径类型
     * @return 旧路径类型
     */
    PathType getOldPathType() const;

private:
    EventType       m_type;
    std::string     m_path;
    PathType        m_pathType;
    std::string     m_oldPath;
    PathType        m_oldPathType;
};

} // namespace filewatch

#endif // FILEWATCH_EVENT_H