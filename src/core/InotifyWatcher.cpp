#include "core/PlatformWatcher.h"
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <cstring>

#ifdef __linux__
#include <sys/inotify.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

namespace filewatch {

class InotifyWatcher : public PlatformWatcher {
private:
    int m_epollFd;
    int m_inotifyFd;
    int m_wakeupFd;

    // 记录用户显式添加的根监控任务及其关联的所有 wd（根自身以及派生的所有子目录）
    struct RootWatch {
        std::string rootPath;
        bool recursive;
        std::unordered_set<int> associatedWds;
    };

    std::unordered_map<int, std::string>                     m_watchMap;         // watch descriptor -> path
    std::unordered_map<std::string, int>                     m_pathWatchMap;     // path -> watch descriptor
    std::unordered_map<int, bool>                            m_watchRecursive;   // watch descriptor -> recursive
    std::unordered_map<std::string, RootWatch>               m_rootWatches;      // rootPath -> RootWatch
    std::unordered_map<int, std::unordered_set<std::string>> m_wdToRoots;        // wd -> rootPaths

    std::function<int64_t()>                m_nextTimeoutProvider;
    std::function<void()>                   m_timerExpiryHandler;
    std::atomic<bool>                       m_running;
    std::function<void(const FileEvent&)>   m_callback;
    std::mutex                              m_mutex;

    // 用于存储重命名事件的旧路径信息
    struct RenameInfo {
        std::string oldPath;
        PathType oldPathType;
    };
    std::unordered_map<uint32_t, RenameInfo> m_renameMap; // cookie -> rename info

    static std::string normalizePath(const std::string& path) {
        std::string p = path;
        while (p.size() > 1 && p.back() == '/') {
            p.pop_back();
        }
        return p;
    }

public:
    explicit InotifyWatcher(std::function<void(const FileEvent&)> callback)
        : m_epollFd(-1), m_inotifyFd(-1), m_wakeupFd(-1), m_running(false), m_callback(callback) {
        m_epollFd = epoll_create1(EPOLL_CLOEXEC);
        m_inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        m_wakeupFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

        if (m_epollFd >= 0) {
            if (m_inotifyFd >= 0) {
                struct epoll_event ev;
                ev.events = EPOLLIN;
                ev.data.fd = m_inotifyFd;
                epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_inotifyFd, &ev);
            }
            if (m_wakeupFd >= 0) {
                struct epoll_event ev;
                ev.events = EPOLLIN;
                ev.data.fd = m_wakeupFd;
                epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_wakeupFd, &ev);
            }
        }
    }

    ~InotifyWatcher() {
        if (m_running) {
            stop();
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& pair : m_watchMap) {
                inotify_rm_watch(m_inotifyFd, pair.first);
            }
            m_watchMap.clear();
            m_pathWatchMap.clear();
            m_watchRecursive.clear();
            m_rootWatches.clear();
            m_wdToRoots.clear();
        }
        if (m_epollFd != -1) {
            close(m_epollFd);
            m_epollFd = -1;
        }
        if (m_inotifyFd != -1) {
            close(m_inotifyFd);
            m_inotifyFd = -1;
        }
        if (m_wakeupFd != -1) {
            close(m_wakeupFd);
            m_wakeupFd = -1;
        }
    }

    void wakeup() override {
        if (m_wakeupFd != -1) {
            uint64_t one = 1;
            ssize_t ret = write(m_wakeupFd, &one, sizeof(one));
            (void)ret;
        }
    }

    void setTimerHandlers(std::function<int64_t()> nextTimeoutProvider,
                          std::function<void()> expiryHandler) override {
        m_nextTimeoutProvider = std::move(nextTimeoutProvider);
        m_timerExpiryHandler = std::move(expiryHandler);
    }

    bool addWatch(const std::string& path, bool recursive) override {
        if (m_inotifyFd == -1) {
            return false;
        }

        const std::string normPath = normalizePath(path);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            RootWatch rw;
            rw.rootPath = normPath;
            rw.recursive = recursive;
            m_rootWatches[normPath] = rw;
        }

        int wd = addDirectoryWatch(normPath, normPath, recursive);
        if (wd == -1) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_rootWatches.erase(normPath);
            return false;
        }

        // 递归监控子目录
        if (recursive) {
            addRecursiveWatches(normPath, normPath);
        }

        return true;
    }

    bool removeWatch(const std::string& path) override {
        if (m_inotifyFd == -1) {
            return false;
        }

        const std::string normPath = normalizePath(path);
        std::vector<int> wdsToRemove;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto rootIt = m_rootWatches.find(normPath);
            if (rootIt != m_rootWatches.end()) {
                // 是显式添加的根监控，遍历其所有派生关联的 wd
                for (int wd : rootIt->second.associatedWds) {
                    auto wdRootsIt = m_wdToRoots.find(wd);
                    if (wdRootsIt != m_wdToRoots.end()) {
                        wdRootsIt->second.erase(normPath);
                        if (wdRootsIt->second.empty()) {
                            m_wdToRoots.erase(wdRootsIt);
                            wdsToRemove.push_back(wd);
                        }
                    }
                }
                m_rootWatches.erase(rootIt);
            } else {
                // 如果未在 rootWatches 中找到，按单个路径兜底处理
                auto pathIt = m_pathWatchMap.find(normPath);
                if (pathIt == m_pathWatchMap.end()) {
                    return false;
                }
                int wd = pathIt->second;
                auto wdRootsIt = m_wdToRoots.find(wd);
                if (wdRootsIt != m_wdToRoots.end()) {
                    m_wdToRoots.erase(wdRootsIt);
                }
                wdsToRemove.push_back(wd);
            }

            // 统一释放已无根任务引用的内核 watch 描述符与内部映射
            for (int wd : wdsToRemove) {
                inotify_rm_watch(m_inotifyFd, wd);
                auto it = m_watchMap.find(wd);
                if (it != m_watchMap.end()) {
                    m_pathWatchMap.erase(it->second);
                    m_watchMap.erase(it);
                }
                m_watchRecursive.erase(wd);
            }
        }

        return true;
    }

    bool start() override {
        if (m_running || m_epollFd == -1 || m_inotifyFd == -1) {
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
    int addDirectoryWatch(const std::string& path, const std::string& rootPath, bool recursive) {
        std::lock_guard<std::mutex> lock(m_mutex);
        int wd = -1;
        auto pathIt = m_pathWatchMap.find(path);
        if (pathIt != m_pathWatchMap.end()) {
            wd = pathIt->second;
        } else {
            wd = inotify_add_watch(m_inotifyFd, path.c_str(),
                IN_CREATE | IN_CLOSE_WRITE | IN_DELETE | IN_DELETE_SELF | IN_MOVED_FROM | IN_MOVED_TO);
            if (wd == -1) {
                return -1;
            }
            m_watchMap[wd] = path;
            m_pathWatchMap[path] = wd;
        }

        m_watchRecursive[wd] = m_watchRecursive[wd] || recursive;
        m_rootWatches[rootPath].associatedWds.insert(wd);
        m_wdToRoots[wd].insert(rootPath);
        return wd;
    }

    void addRecursiveWatches(const std::string& path, const std::string& rootPath) {
        addDirectoryWatch(path, rootPath, true);

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
                addRecursiveWatches(childPath, rootPath);
            }
        }

        closedir(dir);
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
                } else if (fd == m_inotifyFd) {
                    int len = read(m_inotifyFd, buffer, sizeof(buffer));
                    if (len > 0) {
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
            }

            if (m_timerExpiryHandler) {
                m_timerExpiryHandler();
            }
        }

        if (m_timerExpiryHandler) {
            m_timerExpiryHandler();
        }
    }

    void processEvent(struct inotify_event* event) {
        if (event->mask & IN_IGNORED) {
            std::lock_guard<std::mutex> lock(m_mutex);
            const int wd = event->wd;
            auto wIt = m_watchMap.find(wd);
            if (wIt != m_watchMap.end()) {
                m_pathWatchMap.erase(wIt->second);
                m_watchMap.erase(wIt);
            }
            m_watchRecursive.erase(wd);
            auto rIt = m_wdToRoots.find(wd);
            if (rIt != m_wdToRoots.end()) {
                for (const auto& rPath : rIt->second) {
                    auto rootIt = m_rootWatches.find(rPath);
                    if (rootIt != m_rootWatches.end()) {
                        rootIt->second.associatedWds.erase(wd);
                    }
                }
                m_wdToRoots.erase(rIt);
            }
            return;
        }

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
                std::vector<std::string> roots;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    auto it = m_wdToRoots.find(event->wd);
                    if (it != m_wdToRoots.end()) {
                        for (const auto& r : it->second) {
                            auto rwIt = m_rootWatches.find(r);
                            if (rwIt != m_rootWatches.end() && rwIt->second.recursive) {
                                roots.push_back(r);
                            }
                        }
                    }
                }
                for (const auto& r : roots) {
                    addRecursiveWatches(path, r);
                }
                // 目录创建后可能已快速写入文件，做一次现状补采样。
                emitCreateEventsForExistingEntries(path);
            }
        } else if (event->mask & IN_CLOSE_WRITE) {
            // 只有文件保存后才通知 MODIFY
            fileEvent = FileEvent(EventType::kModify, path, pathType);
        } else if (event->mask & (IN_DELETE | IN_DELETE_SELF)) {
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
                std::vector<std::string> roots;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    auto it = m_wdToRoots.find(event->wd);
                    if (it != m_wdToRoots.end()) {
                        for (const auto& r : it->second) {
                            auto rwIt = m_rootWatches.find(r);
                            if (rwIt != m_rootWatches.end() && rwIt->second.recursive) {
                                roots.push_back(r);
                            }
                        }
                    }
                }
                for (const auto& r : roots) {
                    addRecursiveWatches(path, r);
                }
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
