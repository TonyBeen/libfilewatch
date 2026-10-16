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
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace filewatch {

class FanotifyWatcher : public PlatformWatcher {
private:
    int m_epollFd;
    int m_fanotifyFd;
    int m_wakeupFd;
    std::function<int64_t()> m_nextTimeoutProvider;
    std::function<void()> m_timerExpiryHandler;
    std::atomic<bool> m_running;
    std::function<void(const FileEvent&)> m_callback;

public:
    explicit FanotifyWatcher(std::function<void(const FileEvent&)> callback)
        : m_epollFd(-1), m_fanotifyFd(-1), m_wakeupFd(-1), m_running(false), m_callback(callback) {
        m_epollFd = epoll_create1(EPOLL_CLOEXEC);
        m_fanotifyFd = fanotify_init(FAN_CLOEXEC | FAN_NONBLOCK, O_RDONLY);
        m_wakeupFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

        if (m_epollFd >= 0) {
            if (m_fanotifyFd >= 0) {
                struct epoll_event ev;
                ev.events = EPOLLIN;
                ev.data.fd = m_fanotifyFd;
                epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_fanotifyFd, &ev);
            }
            if (m_wakeupFd >= 0) {
                struct epoll_event ev;
                ev.events = EPOLLIN;
                ev.data.fd = m_wakeupFd;
                epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_wakeupFd, &ev);
            }
        }
    }

    ~FanotifyWatcher() {
        if (m_running) {
            stop();
        }
        if (m_epollFd != -1) {
            close(m_epollFd);
            m_epollFd = -1;
        }
        if (m_fanotifyFd != -1) {
            close(m_fanotifyFd);
            m_fanotifyFd = -1;
        }
        if (m_wakeupFd != -1) {
            close(m_wakeupFd);
            m_wakeupFd = -1;
        }
    }

    void wakeup() override {
        if (m_wakeupFd != -1) {
            uint64_t one = 1;
            (void)write(m_wakeupFd, &one, sizeof(one));
        }
    }

    void setTimerHandlers(std::function<int64_t()> nextTimeoutProvider,
                          std::function<void()> expiryHandler) override {
        m_nextTimeoutProvider = std::move(nextTimeoutProvider);
        m_timerExpiryHandler = std::move(expiryHandler);
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
        if (m_running || m_epollFd == -1 || m_fanotifyFd == -1) {
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
        wakeup();
    }

private:
    void watchLoop() {
        char buffer[4096];
        struct epoll_event events[32];

        while (m_running) {
            int64_t timeoutMs = 1000;
            if (m_nextTimeoutProvider) {
                timeoutMs = m_nextTimeoutProvider();
                if (timeoutMs < 0) timeoutMs = 0;
                if (timeoutMs > 1000) timeoutMs = 1000;
            }

            int nfds = epoll_wait(m_epollFd, events, 32, static_cast<int>(timeoutMs));
            if (nfds < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }

            for (int i = 0; i < nfds; ++i) {
                const int fd = events[i].data.fd;
                if (fd == m_wakeupFd) {
                    uint64_t val = 0;
                    while (read(m_wakeupFd, &val, sizeof(val)) > 0) {}
                } else if (fd == m_fanotifyFd) {
                    int len = read(m_fanotifyFd, buffer, sizeof(buffer));
                    if (len > 0) {
                        for (char* p = buffer; p < buffer + len;) {
                            struct fanotify_event_metadata* metadata = reinterpret_cast<struct fanotify_event_metadata*>(p);
                            processEvent(metadata);
                            p += metadata->event_len;
                        }
                    }
                }
            }

            if (m_timerExpiryHandler) {
                m_timerExpiryHandler();
            }
        }

        if (m_timerExpiryHandler) {
            m_timerExpiryHandler();
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
        EventType eventType = EventType::kModify;
        if (metadata->mask & FAN_CREATE) {
            eventType = EventType::kCreate;
        } else if (metadata->mask & FAN_DELETE) {
            eventType = EventType::kDelete;
        } else if (metadata->mask & FAN_CLOSE_WRITE) {
            // 只有文件保存后才通知 MODIFY
            eventType = EventType::kModify;
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
