#include "core/PlatformWatcher.h"
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>
#include <unordered_map>
#include <cstring>

#ifdef __linux__
#include <sys/inotify.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

namespace filewatch {

class InotifyWatcher : public PlatformWatcher {
private:
    int m_inotifyFd;
    std::unordered_map<int, std::string> m_watchMap; // watch descriptor -> path
    std::unordered_map<std::string, int> m_pathWatchMap; // path -> watch descriptor
    std::unordered_map<int, bool> m_watchRecursive; // watch descriptor -> recursive
    std::atomic<bool> m_running;
    std::function<void(const FileEvent&)> m_callback;
    std::mutex m_mutex;

    // 用于存储重命名事件的旧路径信息
    struct RenameInfo {
        std::string oldPath;
        PathType oldPathType;
    };
    std::unordered_map<uint32_t, RenameInfo> m_renameMap; // cookie -> rename info

public:
    explicit InotifyWatcher(std::function<void(const FileEvent&)> callback)
        : m_inotifyFd(-1), m_running(false), m_callback(callback) {
        m_inotifyFd = inotify_init1(IN_NONBLOCK);
    }

    ~InotifyWatcher() {
        if (m_running) {
            stop();
        }
        if (m_inotifyFd != -1) {
            close(m_inotifyFd);
        }
    }

    bool addWatch(const std::string& path, bool recursive) override {
        if (m_inotifyFd == -1) {
            return false;
        }

        int wd = inotify_add_watch(m_inotifyFd, path.c_str(), 
            IN_CREATE | IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
        if (wd == -1) {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_watchMap[wd] = path;
            m_pathWatchMap[path] = wd;
            m_watchRecursive[wd] = recursive;
        }

        // 递归监控子目录
        if (recursive) {
            addRecursiveWatches(path, true);
        }

        return true;
    }

    bool removeWatch(const std::string& path) override {
        if (m_inotifyFd == -1) {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        std::unordered_map<std::string, int>::iterator pathIt = m_pathWatchMap.find(path);
        if (pathIt == m_pathWatchMap.end()) {
            return false;
        }

        const int wd = pathIt->second;
        inotify_rm_watch(m_inotifyFd, wd);
        m_watchRecursive.erase(wd);
        m_watchMap.erase(wd);
        m_pathWatchMap.erase(pathIt);
        return true;
    }

    bool start() override {
        if (m_running || m_inotifyFd == -1) {
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
    bool isPathWatched(const std::string& path) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pathWatchMap.find(path) != m_pathWatchMap.end();
    }

    void addDirectoryWatch(const std::string& path, bool recursive) {
        if (isPathWatched(path)) {
            return;
        }

        int wd = inotify_add_watch(m_inotifyFd, path.c_str(),
            IN_CREATE | IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
        if (wd == -1) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_watchMap[wd] = path;
        m_pathWatchMap[path] = wd;
        m_watchRecursive[wd] = recursive;
    }

    void emitCreateEventsForExistingEntries(const std::string& path) {
        DIR* dir = opendir(path.c_str());
        if (!dir) {
            return;
        }

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            std::string childPath = path + "/" + entry->d_name;
            struct stat statbuf;
            if (stat(childPath.c_str(), &statbuf) != 0) {
                continue;
            }

            if (S_ISDIR(statbuf.st_mode)) {
                if (m_callback) {
                    m_callback(FileEvent(EventType::kCreate, childPath, PathType::DIRECTORY));
                }
                emitCreateEventsForExistingEntries(childPath);
            } else {
                if (m_callback) {
                    m_callback(FileEvent(EventType::kCreate, childPath, PathType::FILE));
                }
            }
        }

        closedir(dir);
    }

    void watchLoop() {
        char buffer[64 * 1024];
        while (m_running) {
            int len = read(m_inotifyFd, buffer, sizeof(buffer));
            if (len == -1) {
                if (errno != EAGAIN) {
                    break;
                }
                // 等待一段时间后重试
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            char* p = buffer;
            char* end = buffer + len;
            while (p + static_cast<int>(sizeof(struct inotify_event)) <= end) {
                struct inotify_event* event = reinterpret_cast<struct inotify_event*>(p);
                const size_t eventSize = sizeof(struct inotify_event) + event->len;
                if (p + static_cast<int>(eventSize) > end) {
                    break;
                }
                processEvent(event);
                p += eventSize;
            }
        }
    }

    void addRecursiveWatches(const std::string& path, bool recursive) {
        // 关键修复：先确保目录本身被监控，再递归子目录。
        addDirectoryWatch(path, recursive);

        DIR* dir = opendir(path.c_str());
        if (!dir) {
            return;
        }

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            // 跳过 . 和 .. 目录
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            std::string childPath = path + "/" + entry->d_name;
            struct stat statbuf;
            if (stat(childPath.c_str(), &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) {
                // 递归处理子目录（函数内部会为子目录本身加 watch）
                addRecursiveWatches(childPath, recursive);
            }
        }

        closedir(dir);
    }

    void processEvent(struct inotify_event* event) {
        std::string path;
        bool recursiveWatch = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_watchMap.find(event->wd);
            if (it == m_watchMap.end()) {
                return;
            }
            path = it->second;
            std::unordered_map<int, bool>::const_iterator recursiveIt = m_watchRecursive.find(event->wd);
            if (recursiveIt != m_watchRecursive.end()) {
                recursiveWatch = recursiveIt->second;
            }
        }

        if (event->len > 0) {
            path += "/" + std::string(event->name);
        }

        PathType pathType = PathType::FILE;
        if (event->mask & IN_ISDIR) {
            pathType = PathType::DIRECTORY;
        }

        FileEvent fileEvent(EventType::kCreate, path, pathType);

        if (event->mask & IN_CREATE) {
            fileEvent = FileEvent(EventType::kCreate, path, pathType);
            // 如果创建的是目录，递归添加监控
            if ((event->mask & IN_ISDIR) && recursiveWatch) {
                addRecursiveWatches(path, true);
                // 目录创建后可能已快速写入文件，做一次现状补采样。
                emitCreateEventsForExistingEntries(path);
            }
        } else if (event->mask & IN_CLOSE_WRITE) {
            // 只有文件保存后才通知 MODIFY
            fileEvent = FileEvent(EventType::kModify, path, pathType);
        } else if (event->mask & IN_DELETE) {
            fileEvent = FileEvent(EventType::kDelete, path, pathType);
        } else if (event->mask & IN_MOVED_FROM) {
            // 存储重命名事件的旧路径信息
            RenameInfo info;
            info.oldPath = path;
            info.oldPathType = pathType;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_renameMap[event->cookie] = info;
            }
            return;
        } else if (event->mask & IN_MOVED_TO) {
            // 检查是否有对应的 IN_MOVED_FROM 事件
            RenameInfo renameInfo;
            bool hasRenameInfo = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_renameMap.find(event->cookie);
                if (it != m_renameMap.end()) {
                    renameInfo = it->second;
                    m_renameMap.erase(it);
                    hasRenameInfo = true;
                }
            }

            if (hasRenameInfo) {
                // 创建重命名事件
                fileEvent = FileEvent(EventType::kRename, path, pathType, renameInfo.oldPath, renameInfo.oldPathType);
            } else {
                // 如果没有对应的 IN_MOVED_FROM 事件，当作创建事件处理
                fileEvent = FileEvent(EventType::kCreate, path, pathType);
            }
            // 如果移动的是目录，递归添加监控
            if ((event->mask & IN_ISDIR) && recursiveWatch) {
                addRecursiveWatches(path, true);
                // 目录整体移动进来时，补发子树 CREATE 事件，避免漏报。
                emitCreateEventsForExistingEntries(path);
            }
        }

        if (m_callback) {
            m_callback(fileEvent);
        }
    }
};

std::unique_ptr<PlatformWatcher> createInotifyWatcher(std::function<void(const FileEvent&)> callback) {
    return std::unique_ptr<PlatformWatcher>(new InotifyWatcher(callback));
}

} // namespace filewatch

#endif // __linux__
