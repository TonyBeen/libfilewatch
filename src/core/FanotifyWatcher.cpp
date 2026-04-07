#include "core/PlatformWatcher.h"
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
#include <cstring>
#include <sys/stat.h>

#ifdef __linux__
#include <sys/fanotify.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace filewatch {

class FanotifyWatcher : public PlatformWatcher {
private:
    int m_fanotifyFd;
    std::atomic<bool> m_running;
    std::function<void(const FileEvent&)> m_callback;

public:
    explicit FanotifyWatcher(std::function<void(const FileEvent&)> callback)
        : m_fanotifyFd(-1), m_running(false), m_callback(callback) {
        m_fanotifyFd = fanotify_init(FAN_CLOEXEC | FAN_NONBLOCK, O_RDONLY);
    }

    ~FanotifyWatcher() {
        if (m_running) {
            stop();
        }
        if (m_fanotifyFd != -1) {
            close(m_fanotifyFd);
        }
    }

    bool addWatch(const std::string& path, bool recursive) override {
        if (m_fanotifyFd == -1) {
            return false;
        }

        if (fanotify_mark(m_fanotifyFd, FAN_MARK_ADD | FAN_MARK_MOUNT, 
            FAN_CLOSE_WRITE | FAN_CREATE | FAN_DELETE, 
            AT_FDCWD, path.c_str()) < 0) {
            return false;
        }

        return true;
    }

    bool removeWatch(const std::string& path) override {
        if (m_fanotifyFd == -1) {
            return false;
        }

        if (fanotify_mark(m_fanotifyFd, FAN_MARK_REMOVE | FAN_MARK_MOUNT, 
            FAN_CLOSE_WRITE | FAN_CREATE | FAN_DELETE, 
            AT_FDCWD, path.c_str()) < 0) {
            return false;
        }

        return true;
    }

    bool start() override {
        if (m_running || m_fanotifyFd == -1) {
            return false;
        }

        m_running = true;
        watchLoop();
        return true;
    }

    void stop() override {
        if (!m_running) {
            return;
        }

        m_running = false;
    }

private:
    void watchLoop() {
        char buffer[4096];
        while (m_running) {
            int len = read(m_fanotifyFd, buffer, sizeof(buffer));
            if (len == -1) {
                if (errno != EAGAIN) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            // 处理 fanotify 事件
            struct fanotify_event_metadata* metadata;
            for (char* p = buffer; p < buffer + len;) {
                metadata = reinterpret_cast<struct fanotify_event_metadata*>(p);
                processEvent(metadata);
                p += metadata->event_len;
            }
        }
    }

    void processEvent(struct fanotify_event_metadata* metadata) {
        // 获取文件路径
        char path[PATH_MAX];
        ssize_t path_len;

        // 使用 /proc/self/fd/ 路径获取文件路径
        snprintf(path, sizeof(path), "/proc/self/fd/%d", metadata->fd);
        path_len = readlink(path, path, sizeof(path) - 1);
        if (path_len == -1) {
            return;
        }
        path[path_len] = '\0';

        std::string filePath = path;
        PathType pathType = PathType::FILE;

        // 检查是否为目录
        struct stat stat_buf;
        if (fstat(metadata->fd, &stat_buf) == 0) {
            if (S_ISDIR(stat_buf.st_mode)) {
                pathType = PathType::DIRECTORY;
            }
        }

        // 关闭文件描述符
        close(metadata->fd);

        // 解析事件类型
        EventType eventType = EventType::MODIFY;
        if (metadata->mask & FAN_CREATE) {
            eventType = EventType::CREATE;
        } else if (metadata->mask & FAN_DELETE) {
            eventType = EventType::DELETE;
        } else if (metadata->mask & FAN_CLOSE_WRITE) {
            // 只有文件保存后才通知 MODIFY
            eventType = EventType::MODIFY;
        } else {
            // 其他事件（如属性修改、文件访问等）可以忽略
            return;
        }

        // 创建 FileEvent 对象
        FileEvent fileEvent(eventType, filePath, pathType);

        // 调用回调函数
        if (m_callback) {
            m_callback(fileEvent);
        }
    }
};

std::unique_ptr<PlatformWatcher> createFanotifyWatcher(std::function<void(const FileEvent&)> callback) {
    return std::unique_ptr<PlatformWatcher>(new FanotifyWatcher(callback));
}

} // namespace filewatch

#endif // __linux__
