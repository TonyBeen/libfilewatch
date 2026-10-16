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
#include <sys/event.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>

namespace filewatch {

class MacOSWatcher : public PlatformWatcher {
private:
    static const uintptr_t WAKEUP_IDENT = 1;

    // 递归监控列表（使用 FSEvents）
    std::vector<std::string> m_fseventsWatchList;
    FSEventStreamRef m_eventStream;
    std::unordered_set<std::string> m_knownPaths;
    std::unordered_map<std::string, PathType> m_knownPathTypes;
    std::mutex m_knownPathsMutex;

    // 非递归监控项（使用主线程 kqueue 原生监听）
    struct DirectChildSnapshot {
        PathType type;
        time_t mtime;
        off_t size;
    };

    struct KqItem {
        std::string path;
        int fd;
        bool isDirectory;
        std::unordered_map<std::string, DirectChildSnapshot> children;
        std::unordered_map<std::string, int> childFileFds; // 直接子文件的 fd，用于监听修改
    };

    int m_kqFd;
    std::unordered_map<std::string, std::shared_ptr<KqItem> > m_kqWatches;
    std::unordered_map<int, std::shared_ptr<KqItem> > m_kqFdMap; // 包含主 fd 和直接子文件 fd
    std::unordered_map<int, std::string> m_childFdToPath; // 子文件 fd 映射到路径
    std::mutex m_kqMutex;

    // FSEvents 异步事件队列（主线程中消费派发）
    std::vector<FileEvent> m_pendingEvents;
    std::mutex m_eventQueueMutex;

    std::function<int64_t()> m_nextTimeoutProvider;
    std::function<void()> m_timerExpiryHandler;

    std::atomic<bool> m_running;
    std::function<void(const FileEvent&)> m_callback;
    std::mutex m_mutex;

public:
    explicit MacOSWatcher(std::function<void(const FileEvent&)> callback)
        : m_eventStream(nullptr),
          m_kqFd(-1),
          m_running(false),
          m_callback(callback) {
        m_kqFd = kqueue();
        if (m_kqFd >= 0) {
            struct kevent change;
            EV_SET(&change, WAKEUP_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
            kevent(m_kqFd, &change, 1, nullptr, 0, nullptr);
        }
    }

    ~MacOSWatcher() {
        if (m_running) {
            stop();
        }
        closeKqueue();
    }

    void wakeup() override {
        if (m_kqFd >= 0) {
            struct kevent trigger;
            EV_SET(&trigger, WAKEUP_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
            kevent(m_kqFd, &trigger, 1, nullptr, 0, nullptr);
        }
    }

    void setTimerHandlers(std::function<int64_t()> nextTimeoutProvider,
                          std::function<void()> expiryHandler) override {
        m_nextTimeoutProvider = std::move(nextTimeoutProvider);
        m_timerExpiryHandler = std::move(expiryHandler);
    }

    bool addWatch(const std::string& path, bool recursive) override {
        struct stat stat_buf;
        if (stat(path.c_str(), &stat_buf) != 0) {
            return false; // 路径不存在
        }

        if (access(path.c_str(), R_OK) != 0) {
            return false; // 没有读权限
        }

        const bool isDir = S_ISDIR(stat_buf.st_mode);

        std::lock_guard<std::mutex> lock(m_mutex);
        if (recursive && isDir) {
            // 递归目录：使用 FSEvents 引擎
            m_fseventsWatchList.push_back(path);
            rememberExistingPathTree(path);
            if (m_running) {
                stopFSEvents();
                startFSEvents();
            }
        } else {
            // 非递归目录或单文件：使用 kqueue 原生监听
            if (!addKqueueItem(path, isDir)) {
                return false;
            }
        }
        return true;
    }

    bool removeWatch(const std::string& path) override {
        std::lock_guard<std::mutex> lock(m_mutex);

        // 先尝试从 FSEvents 列表中移除
        auto itFs = std::find(m_fseventsWatchList.begin(), m_fseventsWatchList.end(), path);
        if (itFs != m_fseventsWatchList.end()) {
            m_fseventsWatchList.erase(itFs);
            if (m_running) {
                stopFSEvents();
                startFSEvents();
            }
            return true;
        }

        // 再尝试从 kqueue 列表中移除
        return removeKqueueItem(path);
    }

    bool start() override {
        if (m_running || m_kqFd < 0) {
            return false;
        }

        m_running = true;

        // 进入主事件循环（由当前主线程执行，零额外独占线程）
        watchLoop();

        return true;
    }

    void stop() override {
        if (!m_running) {
            return;
        }

        m_running = false;

        // 停止 FSEvents 流
        stopFSEvents();

        // 唤醒主线程退出 kevent
        wakeup();
    }

private:
    void pushEventToMainThread(const FileEvent& event) {
        {
            std::lock_guard<std::mutex> lock(m_eventQueueMutex);
            m_pendingEvents.push_back(event);
        }
        wakeup();
    }

    void pushEventsToMainThread(const std::vector<FileEvent>& events) {
        if (events.empty()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_eventQueueMutex);
            m_pendingEvents.insert(m_pendingEvents.end(), events.begin(), events.end());
        }
        wakeup();
    }

    void dispatchPendingEventsOnMainThread() {
        std::vector<FileEvent> events;
        {
            std::lock_guard<std::mutex> lock(m_eventQueueMutex);
            events.swap(m_pendingEvents);
        }
        for (const auto& ev : events) {
            if (m_callback) {
                m_callback(ev);
            }
        }
    }

    // ==========================================
    // kqueue 子引擎实现（专管非递归目录及单文件）
    // ==========================================
    static std::unordered_map<std::string, DirectChildSnapshot> scanDirectChildren(const std::string& dirPath) {
        std::unordered_map<std::string, DirectChildSnapshot> snapshot;
        DIR* dir = opendir(dirPath.c_str());
        if (!dir) {
            return snapshot;
        }

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            std::string childPath = dirPath;
            if (!childPath.empty() && childPath[childPath.size() - 1] != '/') {
                childPath += "/";
            }
            childPath += entry->d_name;

            struct stat st;
            if (lstat(childPath.c_str(), &st) == 0) {
                DirectChildSnapshot snap;
                snap.type = S_ISDIR(st.st_mode) ? PathType::DIRECTORY : PathType::FILE;
                snap.mtime = st.st_mtime;
                snap.size = st.st_size;
                snapshot[childPath] = snap;
            }
        }
        closedir(dir);
        return snapshot;
    }

    void syncDirectChildFileWatches(const std::shared_ptr<KqItem>& item) {
        if (!item->isDirectory || m_kqFd < 0) {
            return;
        }

        // 1. 打开新直接子文件的监听（仅直接子文件，绝不递归打开子目录）
        for (const auto& pair : item->children) {
            const std::string& childPath = pair.first;
            if (pair.second.type == PathType::FILE) {
                if (item->childFileFds.find(childPath) == item->childFileFds.end()) {
                    int cfd = open(childPath.c_str(), O_RDONLY | O_EVTONLY);
                    if (cfd >= 0) {
                        item->childFileFds[childPath] = cfd;
                        m_kqFdMap[cfd] = item;
                        m_childFdToPath[cfd] = childPath;

                        struct kevent change;
                        EV_SET(&change, cfd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
                               NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB, 0, item.get());
                        kevent(m_kqFd, &change, 1, nullptr, 0, nullptr);
                    }
                }
            }
        }

        // 2. 清理已不存在的子文件监听
        auto it = item->childFileFds.begin();
        while (it != item->childFileFds.end()) {
            if (item->children.find(it->first) == item->children.end()) {
                int cfd = it->second;
                if (cfd >= 0) {
                    struct kevent change;
                    EV_SET(&change, cfd, EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
                    kevent(m_kqFd, &change, 1, nullptr, 0, nullptr);
                    close(cfd);
                    m_kqFdMap.erase(cfd);
                    m_childFdToPath.erase(cfd);
                }
                it = item->childFileFds.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool addKqueueItem(const std::string& path, bool isDirectory) {
        int fd = open(path.c_str(), O_RDONLY | O_EVTONLY);
        if (fd < 0) {
            return false;
        }

        auto item = std::make_shared<KqItem>();
        item->path = path;
        item->fd = fd;
        item->isDirectory = isDirectory;
        if (isDirectory) {
            item->children = scanDirectChildren(path);
        }

        std::lock_guard<std::mutex> lock(m_kqMutex);
        m_kqWatches[path] = item;
        m_kqFdMap[fd] = item;

        if (m_kqFd >= 0) {
            registerKevent(item);
            if (isDirectory) {
                syncDirectChildFileWatches(item);
            }
        }
        return true;
    }

    bool removeKqueueItem(const std::string& path) {
        std::lock_guard<std::mutex> lock(m_kqMutex);
        auto it = m_kqWatches.find(path);
        if (it == m_kqWatches.end()) {
            return false;
        }

        auto item = it->second;
        // 关闭该目录下的所有子文件 fd
        for (const auto& cpair : item->childFileFds) {
            int cfd = cpair.second;
            if (cfd >= 0) {
                if (m_kqFd >= 0) {
                    struct kevent change;
                    EV_SET(&change, cfd, EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
                    kevent(m_kqFd, &change, 1, nullptr, 0, nullptr);
                }
                close(cfd);
                m_kqFdMap.erase(cfd);
                m_childFdToPath.erase(cfd);
            }
        }
        item->childFileFds.clear();

        // 关闭主 fd
        if (m_kqFd >= 0 && item->fd >= 0) {
            struct kevent change;
            EV_SET(&change, item->fd, EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
            kevent(m_kqFd, &change, 1, nullptr, 0, nullptr);
        }
        if (item->fd >= 0) {
            close(item->fd);
            m_kqFdMap.erase(item->fd);
            item->fd = -1;
        }
        m_kqWatches.erase(it);
        return true;
    }

    void registerKevent(const std::shared_ptr<KqItem>& item) {
        struct kevent change;
        unsigned int fflags = NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE;
        if (!item->isDirectory) {
            fflags |= NOTE_EXTEND | NOTE_ATTRIB;
        }
        EV_SET(&change, item->fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR, fflags, 0, item.get());
        kevent(m_kqFd, &change, 1, nullptr, 0, nullptr);
    }

    void closeKqueue() {
        std::lock_guard<std::mutex> lock(m_kqMutex);
        for (const auto& pair : m_kqWatches) {
            for (const auto& cpair : pair.second->childFileFds) {
                if (cpair.second >= 0) {
                    close(cpair.second);
                }
            }
            pair.second->childFileFds.clear();
            if (pair.second->fd >= 0) {
                close(pair.second->fd);
                pair.second->fd = -1;
            }
        }
        m_kqWatches.clear();
        m_kqFdMap.clear();
        m_childFdToPath.clear();

        if (m_kqFd >= 0) {
            close(m_kqFd);
            m_kqFd = -1;
        }
    }

    void handleKevent(const struct kevent& ev) {
        const int triggeredFd = static_cast<int>(ev.ident);
        std::shared_ptr<KqItem> item;
        std::string childFilePath;
        bool isChildFile = false;

        {
            std::lock_guard<std::mutex> lock(m_kqMutex);
            auto itChild = m_childFdToPath.find(triggeredFd);
            if (itChild != m_childFdToPath.end()) {
                isChildFile = true;
                childFilePath = itChild->second;
            }

            auto it = m_kqFdMap.find(triggeredFd);
            if (it == m_kqFdMap.end()) {
                return;
            }
            item = it->second;
        }

        if (!item) {
            return;
        }

        std::vector<FileEvent> batch;

        if (isChildFile) {
            // 直接子文件的内容被修改
            if (ev.fflags & (NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB)) {
                batch.emplace_back(EventType::kModify, childFilePath, PathType::FILE);
            }
        } else if (item->isDirectory) {
            // 目录自身的子项创建/删除/重命名
            if (ev.fflags & NOTE_WRITE) {
                auto newSnapshot = scanDirectChildren(item->path);
                diffDirectorySnapshot(item, newSnapshot, batch);

                std::lock_guard<std::mutex> lock(m_kqMutex);
                syncDirectChildFileWatches(item);
            }

            if (ev.fflags & NOTE_DELETE) {
                batch.emplace_back(EventType::kDelete, item->path, PathType::DIRECTORY);
            }
        } else {
            // 独立单文件监控项
            if (ev.fflags & (NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB)) {
                batch.emplace_back(EventType::kModify, item->path, PathType::FILE);
            }
            if (ev.fflags & NOTE_RENAME) {
                batch.emplace_back(EventType::kRename, item->path, PathType::FILE);
            }
            if (ev.fflags & NOTE_DELETE) {
                batch.emplace_back(EventType::kDelete, item->path, PathType::FILE);
            }
        }

        if (!batch.empty()) {
            pushEventsToMainThread(batch);
        }
    }

    void diffDirectorySnapshot(const std::shared_ptr<KqItem>& item,
                               const std::unordered_map<std::string, DirectChildSnapshot>& newSnap,
                               std::vector<FileEvent>& batch) {
        const auto& oldSnap = item->children;

        // 1. 查找新增项
        std::vector<std::string> createdPaths;
        for (const auto& pair : newSnap) {
            if (oldSnap.find(pair.first) == oldSnap.end()) {
                createdPaths.push_back(pair.first);
            }
        }

        // 2. 查找删除项
        std::vector<std::string> deletedPaths;
        for (const auto& pair : oldSnap) {
            if (newSnap.find(pair.first) == newSnap.end()) {
                deletedPaths.push_back(pair.first);
            }
        }

        // 3. 匹配重命名
        if (createdPaths.size() == 1 && deletedPaths.size() == 1 &&
            newSnap.at(createdPaths[0]).type == oldSnap.at(deletedPaths[0]).type) {
            const std::string& newP = createdPaths[0];
            const std::string& oldP = deletedPaths[0];
            const PathType pType = newSnap.at(newP).type;
            batch.emplace_back(EventType::kRename, newP, pType, oldP, pType);
        } else {
            for (const auto& p : createdPaths) {
                batch.emplace_back(EventType::kCreate, p, newSnap.at(p).type);
            }
            for (const auto& p : deletedPaths) {
                batch.emplace_back(EventType::kDelete, p, oldSnap.at(p).type);
            }
        }

        // 4. 查找未打开直接监听项的修改
        for (const auto& pair : newSnap) {
            auto itOld = oldSnap.find(pair.first);
            if (itOld != oldSnap.end()) {
                if (pair.second.type == PathType::FILE) {
                    if (pair.second.mtime != itOld->second.mtime || pair.second.size != itOld->second.size) {
                        batch.emplace_back(EventType::kModify, pair.first, PathType::FILE);
                    }
                }
            }
        }

        // 更新快照
        item->children = newSnap;
    }

    // ==========================================
    // FSEvents 子引擎实现（专管递归整树监控）
    // ==========================================
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

    void rememberPath(const std::string& path, PathType pathType) {
        std::lock_guard<std::mutex> lock(m_knownPathsMutex);
        m_knownPaths.insert(path);
        m_knownPathTypes[path] = pathType;
    }

    void forgetPath(const std::string& path) {
        std::lock_guard<std::mutex> lock(m_knownPathsMutex);
        m_knownPaths.erase(path);
        m_knownPathTypes.erase(path);

        auto knownIt = m_knownPaths.begin();
        while (knownIt != m_knownPaths.end()) {
            if (isPathPrefixOf(path, *knownIt)) {
                knownIt = m_knownPaths.erase(knownIt);
            } else {
                ++knownIt;
            }
        }

        auto typeIt = m_knownPathTypes.begin();
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
        auto it = m_knownPathTypes.find(path);
        if (it == m_knownPathTypes.end()) {
            return false;
        }
        pathType = it->second;
        return true;
    }

    std::vector<std::pair<std::string, PathType> > collectKnownDescendantsForDelete(const std::string& dirPath) {
        std::vector<std::pair<std::string, PathType> > descendants;
        std::lock_guard<std::mutex> lock(m_knownPathsMutex);
        for (auto it = m_knownPathTypes.begin(); it != m_knownPathTypes.end(); ++it) {
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

    void startFSEvents() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_fseventsWatchList.empty()) {
            return;
        }

        CFMutableArrayRef pathArray = CFArrayCreateMutable(nullptr, m_fseventsWatchList.size(), &kCFTypeArrayCallBacks);
        if (!pathArray) {
            return;
        }

        for (const std::string& path : m_fseventsWatchList) {
            CFStringRef cfPath = CFStringCreateWithCString(nullptr, path.c_str(), kCFStringEncodingUTF8);
            if (cfPath) {
                CFArrayAppendValue(pathArray, cfPath);
                CFRelease(cfPath);
            }
        }

        FSEventStreamContext context = {0};
        context.info = this;

        m_eventStream = FSEventStreamCreate(
            nullptr,
            &MacOSWatcher::eventCallback,
            &context,
            pathArray,
            kFSEventStreamEventIdSinceNow,
            0.1, // 延迟 0.1s
            kFSEventStreamCreateFlagFileEvents
        );

        CFRelease(pathArray);

        if (m_eventStream) {
            FSEventStreamSetDispatchQueue(m_eventStream, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
            FSEventStreamStart(m_eventStream);
        }
    }

    void stopFSEvents() {
        if (m_eventStream) {
            FSEventStreamStop(m_eventStream);
            FSEventStreamInvalidate(m_eventStream);
            FSEventStreamRelease(m_eventStream);
            m_eventStream = nullptr;
        }
    }

    static void eventCallback(ConstFSEventStreamRef streamRef, void* clientCallBackInfo, size_t numEvents, void* eventPaths, const FSEventStreamEventFlags eventFlags[], const FSEventStreamEventId eventIds[]) {
        (void)streamRef;
        (void)eventIds;
        MacOSWatcher* watcher = static_cast<MacOSWatcher*>(clientCallBackInfo);
        watcher->processFSEvents(numEvents, eventPaths, eventFlags);
    }

    void processFSEvents(size_t numEvents, void* eventPaths, const FSEventStreamEventFlags eventFlags[]) {
        char** paths = static_cast<char**>(eventPaths);
        std::vector<std::pair<std::string, PathType> > pendingRenameSources;
        std::vector<FileEvent> batch;

        for (size_t i = 0; i < numEvents; ++i) {
            std::string path = paths[i];
            EventType eventType = EventType::kModify;
            PathType pathType = PathType::FILE;
            std::string oldPath;
            PathType oldPathType = PathType::FILE;
            bool hasOldPath = false;

            const FSEventStreamEventFlags flags = eventFlags[i];
            const bool seenBefore = hasSeenPath(path);

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

            if (flags & kFSEventStreamEventFlagItemRemoved) {
                eventType = EventType::kDelete;

                if (pathType == PathType::DIRECTORY) {
                    std::vector<std::pair<std::string, PathType> > descendants = collectKnownDescendantsForDelete(path);
                    for (size_t j = 0; j < descendants.size(); ++j) {
                        batch.emplace_back(EventType::kDelete, descendants[j].first, descendants[j].second);
                    }
                }

                forgetPath(path);
            } else if (flags & kFSEventStreamEventFlagItemRenamed) {
                struct stat stat_buf;
                const bool pathExists = (stat(path.c_str(), &stat_buf) == 0);

                if (!pathExists) {
                    pendingRenameSources.push_back(std::make_pair(path, pathType));
                    continue;
                } else if (!seenBefore) {
                    size_t matchedIndex = pendingRenameSources.size();
                    for (size_t j = 0; j < pendingRenameSources.size(); ++j) {
                        if (pendingRenameSources[j].second == pathType) {
                            matchedIndex = j;
                            break;
                        }
                    }

                    if (matchedIndex < pendingRenameSources.size()) {
                        eventType = EventType::kRename;
                        oldPath = pendingRenameSources[matchedIndex].first;
                        oldPathType = pendingRenameSources[matchedIndex].second;
                        hasOldPath = true;
                        pendingRenameSources.erase(pendingRenameSources.begin() + matchedIndex);
                        forgetPath(oldPath);
                        rememberPath(path, pathType);
                    } else {
                        eventType = EventType::kCreate;
                        rememberPath(path, pathType);
                    }
                } else {
                    eventType = EventType::kRename;
                    rememberPath(path, pathType);
                }
            } else if (flags & kFSEventStreamEventFlagItemCreated) {
                eventType = seenBefore ? EventType::kModify : EventType::kCreate;
                rememberPath(path, pathType);
            } else if ((flags & kFSEventStreamEventFlagItemModified) ||
                       (flags & kFSEventStreamEventFlagItemInodeMetaMod) ||
                       (flags & kFSEventStreamEventFlagItemFinderInfoMod) ||
                       (flags & kFSEventStreamEventFlagItemChangeOwner) ||
                       (flags & kFSEventStreamEventFlagItemXattrMod)) {
                eventType = EventType::kModify;
                rememberPath(path, pathType);
            }

            if (pathType == PathType::DIRECTORY && eventType == EventType::kModify) {
                continue;
            }

            batch.emplace_back(eventType, path, pathType, hasOldPath ? oldPath : "", oldPathType);
        }

        for (size_t i = 0; i < pendingRenameSources.size(); ++i) {
            const std::string& oldPath = pendingRenameSources[i].first;
            const PathType oldType = pendingRenameSources[i].second;

            if (oldType == PathType::DIRECTORY) {
                std::vector<std::pair<std::string, PathType> > descendants = collectKnownDescendantsForDelete(oldPath);
                for (size_t j = 0; j < descendants.size(); ++j) {
                    batch.emplace_back(EventType::kDelete, descendants[j].first, descendants[j].second);
                }
            }

            forgetPath(oldPath);
            batch.emplace_back(EventType::kDelete, oldPath, oldType);
        }

        if (!batch.empty()) {
            pushEventsToMainThread(batch);
        }
    }

    // ==========================================
    // 主线程统一事件循环（以 kevent 为唯一核心）
    // ==========================================
    void watchLoop() {
        startFSEvents();

        struct kevent events[64];

        while (m_running) {
            if (m_kqFd < 0) {
                break;
            }

            int64_t timeoutMs = 1000;
            if (m_nextTimeoutProvider) {
                timeoutMs = m_nextTimeoutProvider();
                if (timeoutMs < 0) timeoutMs = 0;
                if (timeoutMs > 1000) timeoutMs = 1000;
            }

            struct timespec ts;
            ts.tv_sec = timeoutMs / 1000;
            ts.tv_nsec = (timeoutMs % 1000) * 1000000;

            int nev = kevent(m_kqFd, nullptr, 0, events, 64, &ts);
            if (nev < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }

            // 在当前主线程中原地处理所有触发的内核 VNODE 事件
            for (int i = 0; i < nev; ++i) {
                if (events[i].filter == EVFILT_VNODE) {
                    handleKevent(events[i]);
                }
                // 若 events[i].filter == EVFILT_USER，由 wakeup() 触发，用于唤醒循环
            }

            // 统一在主线程中按序派发所有事件（来自 VNODE 与后台 FSEvents）
            dispatchPendingEventsOnMainThread();

            // 统一在主线程中按序派发到期定时器
            if (m_timerExpiryHandler) {
                m_timerExpiryHandler();
            }
        }

        // 退出前排空剩余未派发事件与到期定时器
        dispatchPendingEventsOnMainThread();
        if (m_timerExpiryHandler) {
            m_timerExpiryHandler();
        }
    }
};

std::unique_ptr<PlatformWatcher> createMacOSWatcher(std::function<void(const FileEvent&)> callback) {
    return std::unique_ptr<PlatformWatcher>(new MacOSWatcher(callback));
}

} // namespace filewatch

#endif // __APPLE__
