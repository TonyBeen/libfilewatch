#include "core/PlatformWatcher.h"
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>
#include <thread>
#include <functional>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <cstring>
#include <sys/stat.h>
#include <dirent.h>

#ifdef __APPLE__
#include <CoreServices/CoreServices.h>

namespace filewatch {

class MacOSWatcher : public PlatformWatcher {
private:
    std::vector<std::string> m_watchList;
    std::atomic<bool> m_running;
    std::function<void(const FileEvent&)> m_callback;
    std::mutex m_mutex;
    FSEventStreamRef m_eventStream;
    std::unordered_set<std::string> m_knownPaths;
    std::unordered_map<std::string, PathType> m_knownPathTypes;
    std::mutex m_knownPathsMutex;

    // 事件过滤器
    struct Filter {
        std::vector<EventType> eventTypes;
        std::vector<std::string> extensions;

        bool passes(const FileEvent& event) const {
            // 检查事件类型
            if (!eventTypes.empty()) {
                if (std::find(eventTypes.begin(), eventTypes.end(), event.getType()) == eventTypes.end()) {
                    return false;
                }
            }

            // 检查文件扩展名
            if (!extensions.empty() && event.getPathType() == PathType::FILE) {
                const std::string& path = event.getPath();
                size_t dotPos = path.rfind('.');
                if (dotPos != std::string::npos) {
                    std::string extension = path.substr(dotPos);
                    if (std::find(extensions.begin(), extensions.end(), extension) == extensions.end()) {
                        return false;
                    }
                } else {
                    return false; // 没有扩展名的文件
                }
            }

            return true;
        }
    };
    Filter m_filter;

public:
    explicit MacOSWatcher(std::function<void(const FileEvent&)> callback)
        : m_running(false), m_callback(callback), m_eventStream(nullptr) {
    }

    ~MacOSWatcher() {
        if (m_running) {
            stop();
        }
    }

    bool addWatch(const std::string& path, bool recursive) override {
        // 检查路径是否存在
        struct stat stat_buf;
        if (stat(path.c_str(), &stat_buf) != 0) {
            return false; // 路径不存在
        }

        // 检查是否有读权限
        if (access(path.c_str(), R_OK) != 0) {
            return false; // 没有读权限
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_watchList.push_back(path);
        rememberExistingPathTree(path);
        return true;
    }

    bool removeWatch(const std::string& path) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = std::find(m_watchList.begin(), m_watchList.end(), path);
        if (it != m_watchList.end()) {
            m_watchList.erase(it);
            return true;
        }
        return false;
    }

    bool start() override {
        if (m_running) {
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

        // 停止 FSEvents 流
        if (m_eventStream) {
            FSEventStreamStop(m_eventStream);
            FSEventStreamInvalidate(m_eventStream);
            FSEventStreamRelease(m_eventStream);
            m_eventStream = nullptr;
        }
    }

private:
    static bool isPathPrefixOf(const std::string& parent, const std::string& child) {
        if (child == parent) {
            return true;
        }
        if (child.size() <= parent.size() || child.compare(0, parent.size(), parent) != 0) {
            return false;
        }
        if (!parent.empty() && parent[parent.size() - 1] == '/') {
            return true;
        }
        return child[parent.size()] == '/';
    }

    static bool pathLengthDesc(const std::pair<std::string, PathType>& a,
                               const std::pair<std::string, PathType>& b) {
        if (a.first.size() != b.first.size()) {
            return a.first.size() > b.first.size();
        }
        return a.second == PathType::FILE && b.second == PathType::DIRECTORY;
    }

    void emitEventIfPasses(const FileEvent& fileEvent) {
        if (m_filter.passes(fileEvent) && m_callback) {
            m_callback(fileEvent);
        }
    }

    void rememberPath(const std::string& path, PathType pathType) {
        std::lock_guard<std::mutex> lock(m_knownPathsMutex);
        m_knownPaths.insert(path);
        m_knownPathTypes[path] = pathType;
    }

    void forgetPath(const std::string& path) {
        std::lock_guard<std::mutex> lock(m_knownPathsMutex);
        m_knownPaths.erase(path);
        m_knownPathTypes.erase(path);

        std::unordered_set<std::string>::iterator knownIt = m_knownPaths.begin();
        while (knownIt != m_knownPaths.end()) {
            if (isPathPrefixOf(path, *knownIt)) {
                knownIt = m_knownPaths.erase(knownIt);
            } else {
                ++knownIt;
            }
        }

        std::unordered_map<std::string, PathType>::iterator typeIt = m_knownPathTypes.begin();
        while (typeIt != m_knownPathTypes.end()) {
            if (isPathPrefixOf(path, typeIt->first)) {
                typeIt = m_knownPathTypes.erase(typeIt);
            } else {
                ++typeIt;
            }
        }
    }

    bool hasSeenPath(const std::string& path) {
        std::lock_guard<std::mutex> lock(m_knownPathsMutex);
        return m_knownPaths.find(path) != m_knownPaths.end();
    }

    bool getKnownPathType(const std::string& path, PathType& pathType) {
        std::lock_guard<std::mutex> lock(m_knownPathsMutex);
        std::unordered_map<std::string, PathType>::const_iterator it = m_knownPathTypes.find(path);
        if (it == m_knownPathTypes.end()) {
            return false;
        }
        pathType = it->second;
        return true;
    }

    std::vector<std::pair<std::string, PathType> > collectKnownDescendantsForDelete(const std::string& dirPath) {
        std::vector<std::pair<std::string, PathType> > descendants;
        std::lock_guard<std::mutex> lock(m_knownPathsMutex);
        for (std::unordered_map<std::string, PathType>::const_iterator it = m_knownPathTypes.begin(); it != m_knownPathTypes.end(); ++it) {
            if (it->first != dirPath && isPathPrefixOf(dirPath, it->first)) {
                descendants.push_back(*it);
            }
        }
        std::sort(descendants.begin(), descendants.end(), pathLengthDesc);
        return descendants;
    }

    void rememberExistingPathTree(const std::string& path) {
        struct stat stat_buf;
        if (stat(path.c_str(), &stat_buf) != 0) {
            return;
        }

        const PathType currentType = S_ISDIR(stat_buf.st_mode) ? PathType::DIRECTORY : PathType::FILE;
        rememberPath(path, currentType);

        if (!S_ISDIR(stat_buf.st_mode)) {
            return;
        }

        DIR* dir = opendir(path.c_str());
        if (!dir) {
            return;
        }

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            std::string child = path;
            if (!child.empty() && child[child.size() - 1] != '/') {
                child += "/";
            }
            child += entry->d_name;
            rememberExistingPathTree(child);
        }

        closedir(dir);
    }

    void watchLoop() {
        // 创建 FSEvents 流
        createEventStream();

        // 启动 FSEvents 流
        if (m_eventStream) {
            FSEventStreamSetDispatchQueue(m_eventStream, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
            FSEventStreamStart(m_eventStream);

            // 保持现有阻塞语义，直到 stop() 将 m_running 置为 false
            while (m_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    void createEventStream() {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_watchList.empty()) {
            return;
        }

        // 转换路径为 CFStringRef 数组
        CFArrayRef pathsToWatch = createPathArray();
        if (!pathsToWatch) {
            return;
        }

        // 配置 FSEvents 流
        FSEventStreamContext context = {0};
        context.info = this;

        // 创建 FSEvents 流
        m_eventStream = FSEventStreamCreate(
            nullptr,
            &MacOSWatcher::eventCallback,
            &context,
            pathsToWatch,
            kFSEventStreamEventIdSinceNow,
            0.1, // 事件延迟（秒）
            kFSEventStreamCreateFlagFileEvents
        );

        // 释放路径数组
        CFRelease(pathsToWatch);
    }

    CFArrayRef createPathArray() {
        CFMutableArrayRef pathArray = CFArrayCreateMutable(nullptr, m_watchList.size(), &kCFTypeArrayCallBacks);
        if (!pathArray) {
            return nullptr;
        }

        for (const std::string& path : m_watchList) {
            CFStringRef cfPath = CFStringCreateWithCString(nullptr, path.c_str(), kCFStringEncodingUTF8);
            if (cfPath) {
                CFArrayAppendValue(pathArray, cfPath);
                CFRelease(cfPath);
            }
        }

        return pathArray;
    }

    static void eventCallback(ConstFSEventStreamRef streamRef, void* clientCallBackInfo, size_t numEvents, void* eventPaths, const FSEventStreamEventFlags eventFlags[], const FSEventStreamEventId eventIds[]) {
        MacOSWatcher* watcher = static_cast<MacOSWatcher*>(clientCallBackInfo);
        watcher->processEvents(numEvents, eventPaths, eventFlags);
    }

    void processEvents(size_t numEvents, void* eventPaths, const FSEventStreamEventFlags eventFlags[]) {
        char** paths = static_cast<char**>(eventPaths);
        std::vector<std::pair<std::string, PathType> > pendingRenameSources;

        for (size_t i = 0; i < numEvents; ++i) {
            std::string path = paths[i];
            EventType eventType = EventType::MODIFY;
            PathType pathType = PathType::FILE;
            std::string oldPath;
            PathType oldPathType = PathType::FILE;
            bool hasOldPath = false;

            const FSEventStreamEventFlags flags = eventFlags[i];
            const bool seenBefore = hasSeenPath(path);

            // 优先使用 FSEvents 自带类型标记，删除事件下 stat 可能失败。
            if (flags & kFSEventStreamEventFlagItemIsDir) {
                pathType = PathType::DIRECTORY;
            } else if (flags & kFSEventStreamEventFlagItemIsFile) {
                pathType = PathType::FILE;
            } else {
                struct stat stat_buf;
                if (stat(path.c_str(), &stat_buf) == 0 && S_ISDIR(stat_buf.st_mode)) {
                    pathType = PathType::DIRECTORY;
                } else {
                    PathType knownType = PathType::FILE;
                    if (getKnownPathType(path, knownType)) {
                        pathType = knownType;
                    }
                }
            }

            // 解析事件类型：先删除，再创建，避免组合标记误判。
            if (flags & kFSEventStreamEventFlagItemRemoved) {
                eventType = EventType::DELETE;

                if (pathType == PathType::DIRECTORY) {
                    std::vector<std::pair<std::string, PathType> > descendants = collectKnownDescendantsForDelete(path);
                    for (size_t j = 0; j < descendants.size(); ++j) {
                        FileEvent childDelete(EventType::DELETE, descendants[j].first, descendants[j].second);
                        emitEventIfPasses(childDelete);
                    }
                }

                forgetPath(path);
            } else if (flags & kFSEventStreamEventFlagItemRenamed) {
                struct stat stat_buf;
                const bool pathExists = (stat(path.c_str(), &stat_buf) == 0);

                if (!pathExists) {
                    // 先缓存为 rename old-path，循环末尾若仍未配对再降级为 DELETE。
                    pendingRenameSources.push_back(std::make_pair(path, pathType));
                    continue;
                } else if (!seenBefore) {
                    // 优先尝试与同批次中消失的路径配对成 RENAME。
                    size_t matchedIndex = pendingRenameSources.size();
                    for (size_t j = 0; j < pendingRenameSources.size(); ++j) {
                        if (pendingRenameSources[j].second == pathType) {
                            matchedIndex = j;
                            break;
                        }
                    }

                    if (matchedIndex < pendingRenameSources.size()) {
                        eventType = EventType::RENAME;
                        oldPath = pendingRenameSources[matchedIndex].first;
                        oldPathType = pendingRenameSources[matchedIndex].second;
                        hasOldPath = true;
                        pendingRenameSources.erase(pendingRenameSources.begin() + matchedIndex);
                        forgetPath(oldPath);
                        rememberPath(path, pathType);
                    } else {
                        // 路径存在且首次出现，更接近“新建/移动进入监控范围”。
                        eventType = EventType::CREATE;
                        rememberPath(path, pathType);
                    }
                } else {
                    eventType = EventType::RENAME;
                    rememberPath(path, pathType);
                }
            } else if (flags & kFSEventStreamEventFlagItemCreated) {
                // 某些编辑器在覆盖保存时可能仍带 Created 标记；已存在路径按 MODIFY 处理。
                eventType = seenBefore ? EventType::MODIFY : EventType::CREATE;
                rememberPath(path, pathType);
            } else if ((flags & kFSEventStreamEventFlagItemModified) ||
                       (flags & kFSEventStreamEventFlagItemInodeMetaMod) ||
                       (flags & kFSEventStreamEventFlagItemFinderInfoMod) ||
                       (flags & kFSEventStreamEventFlagItemChangeOwner) ||
                       (flags & kFSEventStreamEventFlagItemXattrMod)) {
                eventType = EventType::MODIFY;
                rememberPath(path, pathType);
            }

            // 目录的元数据变化噪声较多，这里抑制 DIRECTORY + MODIFY 事件。
            if (pathType == PathType::DIRECTORY && eventType == EventType::MODIFY) {
                continue;
            }

            // 创建 FileEvent 对象
            FileEvent fileEvent(eventType, path, pathType, hasOldPath ? oldPath : "", oldPathType);

            emitEventIfPasses(fileEvent);
        }

        // 未配对的 rename old-path 视为删除，保证不会丢失事件。
        for (size_t i = 0; i < pendingRenameSources.size(); ++i) {
            const std::string& oldPath = pendingRenameSources[i].first;
            const PathType oldType = pendingRenameSources[i].second;

            if (oldType == PathType::DIRECTORY) {
                std::vector<std::pair<std::string, PathType> > descendants = collectKnownDescendantsForDelete(oldPath);
                for (size_t j = 0; j < descendants.size(); ++j) {
                    FileEvent childDelete(EventType::DELETE, descendants[j].first, descendants[j].second);
                    emitEventIfPasses(childDelete);
                }
            }

            forgetPath(oldPath);
            FileEvent deleteEvent(EventType::DELETE, oldPath, oldType);
            emitEventIfPasses(deleteEvent);
        }
    }
};

std::unique_ptr<PlatformWatcher> createMacOSWatcher(std::function<void(const FileEvent&)> callback) {
    return std::unique_ptr<PlatformWatcher>(new MacOSWatcher(callback));
}

} // namespace filewatch

#endif // __APPLE__
