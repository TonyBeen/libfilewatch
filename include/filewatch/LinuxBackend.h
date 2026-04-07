#ifndef FILEWATCH_LINUXBACKEND_H
#define FILEWATCH_LINUXBACKEND_H

namespace filewatch {

/**
 * Linux 监控后端类型
 */
enum class LinuxBackend {
    INOTIFY,    // 使用 inotify（默认）
    FANOTIFY    // 使用 fanotify
};

} // namespace filewatch

#endif // FILEWATCH_LINUXBACKEND_H