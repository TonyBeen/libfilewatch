#include "core/PlatformWatcher.h"
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>
#include <algorithm>
#include <sys/stat.h>

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
    CFRunLoopRef m_runLoop;

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
        : m_running(false), m_callback(callback), m_eventStream(nullptr), m_runLoop(nullptr) {
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

        // 停止 run loop
        if (m_runLoop) {
            CFRunLoopStop(m_runLoop);
            m_runLoop = nullptr;
        }
    }

private:
    void watchLoop() {
        // 创建 FSEvents 流
        createEventStream();

        // 启动 FSEvents 流
        if (m_eventStream) {
            FSEventStreamScheduleWithRunLoop(m_eventStream, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
            FSEventStreamStart(m_eventStream);

            // 保存 run loop 引用
            m_runLoop = CFRunLoopGetCurrent();

            // 运行 run loop
            while (m_running) {
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
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
            kFSEventStreamCreateFlagNone
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

        for (size_t i = 0; i < numEvents; ++i) {
            std::string path = paths[i];
            EventType eventType = EventType::MODIFY;
            PathType pathType = PathType::FILE;

            // 检查是否为目录
            struct stat stat_buf;
            if (stat(path.c_str(), &stat_buf) == 0) {
                if (S_ISDIR(stat_buf.st_mode)) {
                    pathType = PathType::DIRECTORY;
                }
            }

            // 解析事件类型
            if (eventFlags[i] & kFSEventStreamEventFlagCreated) {
                eventType = EventType::CREATE;
            } else if (eventFlags[i] & kFSEventStreamEventFlagRemoved) {
                eventType = EventType::DELETE;
            } else if (eventFlags[i] & kFSEventStreamEventFlagModified) {
                eventType = EventType::MODIFY;
            } else if (eventFlags[i] & kFSEventStreamEventFlagRenamed) {
                eventType = EventType::RENAME;
            }

            // 创建 FileEvent 对象
            FileEvent fileEvent(eventType, path, pathType);

            // 检查事件是否通过过滤
            if (m_filter.passes(fileEvent)) {
                // 调用回调函数
                if (m_callback) {
                    m_callback(fileEvent);
                }
            }
        }
    }
};

std::unique_ptr<PlatformWatcher> createMacOSWatcher(std::function<void(const FileEvent&)> callback) {
    return std::unique_ptr<PlatformWatcher>(new MacOSWatcher(callback));
}

} // namespace filewatch

#endif // __APPLE__
