#include "filewatch/FileWatcher.h"
#include "utils/PathUtils.h"
#include "core/PlatformWatcher.h"

#include <memory>
#include <unordered_map>
#include <regex>
#include <mutex>
#include <functional>
#include <vector>
#include <algorithm>
#include <chrono>

namespace {

// C++11 兼容的 make_unique 实现
    template<typename T, typename... Args>
    std::unique_ptr<T> makeUnique(Args&&... args) {
        return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
    }

}

// 实现 Filter 类的方法
filewatch::FileWatcher::Filter& filewatch::FileWatcher::Filter::setEventTypes(const std::vector<EventType>& types) {
    m_eventTypes = types;
    return *this;
}

filewatch::FileWatcher::Filter& filewatch::FileWatcher::Filter::setExtensions(const std::vector<std::string>& extensions) {
    m_extensions = extensions;
    return *this;
}

filewatch::FileWatcher::Filter& filewatch::FileWatcher::Filter::setPathPattern(const std::string& pattern) {
    try {
        m_pathPattern = std::regex(pattern);
        m_hasPathPattern = true;
    } catch (const std::regex_error&) {
        m_hasPathPattern = false;
    }
    return *this;
}

filewatch::FileWatcher::Filter& filewatch::FileWatcher::Filter::addExcludePattern(const std::string& pattern) {
    try {
        m_excludePatterns.emplace_back(pattern);
    } catch (const std::regex_error&) {
    }
    return *this;
}

filewatch::FileWatcher::Filter& filewatch::FileWatcher::Filter::setExcludePatterns(const std::vector<std::string>& patterns) {
    m_excludePatterns.clear();
    for (const auto& pat : patterns) {
        addExcludePattern(pat);
    }
    return *this;
}

filewatch::FileWatcher::Filter& filewatch::FileWatcher::Filter::addExcludePath(const std::string& path) {
    if (!path.empty()) {
        std::string p = path;
        std::replace(p.begin(), p.end(), '\\', '/');
        while (p.size() > 1 && p.back() == '/') {
            p.pop_back();
        }
        m_excludePaths.push_back(p);
    }
    return *this;
}

filewatch::FileWatcher::Filter& filewatch::FileWatcher::Filter::setExcludePaths(const std::vector<std::string>& paths) {
    m_excludePaths.clear();
    for (const auto& p : paths) {
        addExcludePath(p);
    }
    return *this;
}

bool filewatch::FileWatcher::Filter::passes(const FileEvent& event) const {
    const std::string& rawPath = event.getPath();

    // 1. 检查固定排除路径（黑名单）
    if (!m_excludePaths.empty()) {
        std::string normalizedEventPath = rawPath;
        std::replace(normalizedEventPath.begin(), normalizedEventPath.end(), '\\', '/');
        while (normalizedEventPath.size() > 1 && normalizedEventPath.back() == '/') {
            normalizedEventPath.pop_back();
        }

        for (const auto& excludePath : m_excludePaths) {
            if (normalizedEventPath == excludePath) {
                return false;
            }
            if (normalizedEventPath.size() > excludePath.size() &&
                normalizedEventPath.compare(0, excludePath.size(), excludePath) == 0 &&
                (excludePath.back() == '/' || normalizedEventPath[excludePath.size()] == '/')) {
                return false;
            }
        }
    }

    // 2. 检查排除正则模式（黑名单）
    for (const auto& pattern : m_excludePatterns) {
        if (std::regex_search(rawPath, pattern)) {
            return false;
        }
    }

    // 3. 检查事件类型
    if (!m_eventTypes.empty()) {
        bool found = false;
        for (const auto& type : m_eventTypes) {
            if (event.getType() == type) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }

    // 4. 检查文件扩展名
    if (!m_extensions.empty() && event.getPathType() == PathType::FILE) {
        const std::string& path = event.getPath();
        size_t dotPos = path.rfind('.');
        if (dotPos != std::string::npos) {
            std::string extension = path.substr(dotPos);
            bool found = false;
            for (const auto& ext : m_extensions) {
                if (extension == ext) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return false;
            }
        } else {
            return false;
        }
    }

    // 5. 检查路径白名单模式
    if (m_hasPathPattern) {
        if (!std::regex_match(event.getPath(), m_pathPattern)) {
            return false;
        }
    }

    return true;
}

namespace filewatch {

// 实现 errc 命名空间中的函数
namespace errc {
    const char* watcher_error_category::name() const noexcept {
        return "filewatch";
    }

    std::string watcher_error_category::message(int ev) const {
        switch (static_cast<watcher_errc>(ev)) {
            case watcher_errc::success: return "success";
            case watcher_errc::invalid_path: return "invalid path";
            case watcher_errc::path_not_exist: return "path does not exist";
            case watcher_errc::permission_denied: return "permission denied";
            case watcher_errc::platform_error: return "platform error";
            case watcher_errc::regex_error: return "regex error";
            case watcher_errc::internal_error: return "internal error";
            case watcher_errc::already_watching: return "already watching";
            case watcher_errc::watcher_not_started: return "watcher not started";
            case watcher_errc::watcher_already_running: return "watcher already running";
            case watcher_errc::unknown_error: return "unknown error";
            default: return "unknown error";
        }
    }

    const watcher_error_category& watcher_category() noexcept {
        static watcher_error_category instance;
        return instance;
    }

    std::error_code make_error_code(watcher_errc e) noexcept {
        return std::error_code(static_cast<int>(e), watcher_category());
    }
}



class FileWatcher::Impl {
public:
    Impl(LinuxBackend backend) : m_lastError(make_error_code(errc::watcher_errc::success)),
                                m_debounceTime(50),
                                m_debounceCleanupIntervalMs(30000),
                                m_debounceExpireMs(120000),
                                m_lastDebounceCleanup(std::chrono::steady_clock::now()) {
        m_platformWatcher = createPlatformWatcher(backend, [this](const FileEvent& event) {
            processEvent(event);
        });
        m_platformWatcher->setTimerHandlers(
            [this]() -> int64_t {
                return getNextTimeoutMs();
            },
            [this]() {
                processExpiredTimers();
            }
        );
    }

    ~Impl() {
    }

    bool addWatch(const std::string& path, bool recursive, EventCallback callback) {
        if (path.empty()) {
            setError(errc::watcher_errc::invalid_path);
            return false;
        }

        const std::string normalizedPath = utils::PathUtils::getAbsolutePath(path);

        if (!utils::PathUtils::exists(normalizedPath)) {
            setError(errc::watcher_errc::path_not_exist);
            return false;
        }

        try {
            {
                std::lock_guard<std::mutex> lock(m_callbacksMutex);
                if (m_watches.find(normalizedPath) != m_watches.end()) {
                    setError(errc::watcher_errc::already_watching);
                    return false;
                }
                m_watches[normalizedPath] = WatchEntry{callback, recursive};
            }

            if (!m_platformWatcher->addWatch(normalizedPath, recursive)) {
                setError(errc::watcher_errc::platform_error);
                std::lock_guard<std::mutex> lock(m_callbacksMutex);
                m_watches.erase(normalizedPath);
                return false;
            }
            setError(errc::watcher_errc::success);
            return true;
        } catch (const std::exception&) {
            setError(errc::watcher_errc::internal_error);
            return false;
        }
    }

    bool removeWatch(const std::string& path) {
        if (path.empty()) {
            setError(errc::watcher_errc::invalid_path);
            return false;
        }

        const std::string normalizedPath = utils::PathUtils::getAbsolutePath(path);

        try {
            {
                std::lock_guard<std::mutex> lock(m_callbacksMutex);
                m_watches.erase(normalizedPath);
            }
            if (!m_platformWatcher->removeWatch(normalizedPath)) {
                setError(errc::watcher_errc::platform_error);
                return false;
            }
            setError(errc::watcher_errc::success);
            return true;
        } catch (const std::exception&) {
            setError(errc::watcher_errc::internal_error);
            return false;
        }
    }

    bool addWatchWithRegex(const std::string& directory, const std::string& pattern, 
                          bool recursive, EventCallback callback) {
        if (directory.empty()) {
            setError(errc::watcher_errc::invalid_path);
            return false;
        }

        try {
            // 在 add 阶段编译一次正则，避免每个事件重复编译
            std::shared_ptr<std::regex> regexPattern = std::make_shared<std::regex>(pattern);
            return addWatch(directory, recursive, [regexPattern, callback](const FileEvent& event) {
                if (std::regex_match(event.getPath(), *regexPattern)) {
                    callback(event);
                }
            });
        } catch (const std::regex_error&) {
            setError(errc::watcher_errc::regex_error);
            return false;
        }
    }

    bool start() {
        try {
            if (!m_platformWatcher->start()) {
                setError(errc::watcher_errc::platform_error);
                return false;
            }
            setError(errc::watcher_errc::success);
            return true;
        } catch (const std::exception&) {
            setError(errc::watcher_errc::internal_error);
            return false;
        }
    }

    void stop() {
        try {
            m_platformWatcher->stop();
            setError(errc::watcher_errc::success);
        } catch (const std::exception&) {
            setError(errc::watcher_errc::internal_error);
        }
    }

    void setError(errc::watcher_errc code) {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError = make_error_code(code);
    }

    std::error_code getLastError() const {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        return m_lastError;
    }

    bool addTimer(Timer& timer, uint32_t timeoutMs) {
        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(m_timersMutex);
            timer.m_intervalMs = timeoutMs;
            timer.m_expireTime = now + std::chrono::milliseconds(timeoutMs);
            timer.m_active = true;

            auto it = std::find(m_timers.begin(), m_timers.end(), &timer);
            if (it == m_timers.end()) {
                m_timers.push_back(&timer);
            }
        }
        if (m_platformWatcher) {
            m_platformWatcher->wakeup();
        }
        return true;
    }

    bool removeTimer(Timer& timer) {
        {
            std::lock_guard<std::mutex> lock(m_timersMutex);
            timer.m_active = false;
            auto it = std::find(m_timers.begin(), m_timers.end(), &timer);
            if (it != m_timers.end()) {
                m_timers.erase(it);
            }
        }
        if (m_platformWatcher) {
            m_platformWatcher->wakeup();
        }
        return true;
    }

    int64_t getNextTimeoutMs() {
        std::lock_guard<std::mutex> lock(m_timersMutex);
        if (m_timers.empty()) {
            return 1000; // 0 定时器时最长等待 1s
        }

        auto now = std::chrono::steady_clock::now();
        int64_t minDiffMs = 1000;
        bool hasActive = false;

        for (Timer* t : m_timers) {
            if (t && t->m_active) {
                hasActive = true;
                if (t->m_expireTime <= now) {
                    return 0;
                }
                int64_t diff = std::chrono::duration_cast<std::chrono::milliseconds>(t->m_expireTime - now).count();
                if (diff < minDiffMs) {
                    minDiffMs = diff;
                }
            }
        }

        if (!hasActive) {
            return 1000;
        }

        if (minDiffMs < 0) minDiffMs = 0;
        if (minDiffMs > 1000) minDiffMs = 1000;
        return minDiffMs;
    }

    void processExpiredTimers() {
        std::vector<Timer*> expired;
        auto now = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(m_timersMutex);
            for (auto it = m_timers.begin(); it != m_timers.end(); ) {
                Timer* t = *it;
                if (!t || !t->m_active) {
                    it = m_timers.erase(it);
                    continue;
                }

                if (t->m_expireTime <= now) {
                    expired.push_back(t);
                    if (t->m_repeating) {
                        t->m_expireTime = now + std::chrono::milliseconds(t->m_intervalMs);
                        ++it;
                    } else {
                        ++it;
                    }
                } else {
                    ++it;
                }
            }
        }

        // 统一在主线程执行回调
        for (Timer* t : expired) {
            Timer::Callback cb;
            {
                std::lock_guard<std::mutex> lock(m_timersMutex);
                if (!t || !t->m_active) {
                    continue; // 可能在先前的回调中被主动 removeTimer
                }
                if (!t->m_repeating) {
                    t->m_active = false;
                    auto it = std::find(m_timers.begin(), m_timers.end(), t);
                    if (it != m_timers.end()) {
                        m_timers.erase(it);
                    }
                }
                cb = t->m_callback;
            }

            if (cb) {
                try {
                    cb(*t);
                } catch (...) {
                }
            }
        }
    }

    void setFilter(const FileWatcher::Filter& filter) {
        std::lock_guard<std::mutex> lock(m_filterMutex);
        m_filter = filter;
    }

private:
    struct WatchEntry {
        EventCallback callback;
        bool recursive;
    };

    static bool isPathUnderWatch(const std::string& watchPath, const std::string& eventPath, bool recursive) {
        if (watchPath.empty()) {
            return false;
        }

        std::string w = watchPath;
        std::string e = eventPath;
        std::replace(w.begin(), w.end(), '\\', '/');
        std::replace(e.begin(), e.end(), '\\', '/');

        // 去除末尾斜杠
        while (w.size() > 1 && w.back() == '/') {
            w.pop_back();
        }

        if (e == w) {
            return true;
        }
        if (e.size() <= w.size() || e.compare(0, w.size(), w) != 0) {
            return false;
        }
        if (e[w.size()] != '/') {
            return false;
        }

        // recursive = true 时，所有子目录及更深层项均在监控范围内
        if (recursive) {
            return true;
        }

        // recursive = false 时，只放行直接子项（相对路径中不能包含 /）
        std::string relative = e.substr(w.size() + 1);
        return relative.find('/') == std::string::npos;
    }

    std::unique_ptr<PlatformWatcher> m_platformWatcher;
    std::unordered_map<std::string, WatchEntry> m_watches;
    mutable std::mutex m_callbacksMutex;
    std::error_code m_lastError; // 存储最后一个错误
    mutable std::mutex m_errorMutex;
    FileWatcher::Filter m_filter; // 事件过滤器
    mutable std::mutex m_filterMutex;

    // 定时器管理（外部持有对象，零动态内存分配）
    std::vector<Timer*> m_timers;
    mutable std::mutex m_timersMutex;

    // 仅保留防抖，不再使用后台批处理线程
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_debounceMap; // 防抖映射
    std::mutex m_debounceMutex; // 防抖互斥锁
    int m_debounceTime; // 防抖时间（毫秒）
    int m_debounceCleanupIntervalMs; // 防抖表清理间隔
    int m_debounceExpireMs; // 防抖表过期时间
    std::chrono::steady_clock::time_point m_lastDebounceCleanup; // 上次清理时间

    void cleanupDebounceMap(const std::chrono::steady_clock::time_point& now) {
        const int64_t sinceLastCleanup = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastDebounceCleanup).count();
        if (sinceLastCleanup < m_debounceCleanupIntervalMs) {
            return;
        }

        const int64_t expireMs = m_debounceExpireMs;
        for (std::unordered_map<std::string, std::chrono::steady_clock::time_point>::iterator it = m_debounceMap.begin(); it != m_debounceMap.end();) {
            const int64_t age = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
            if (age > expireMs) {
                it = m_debounceMap.erase(it);
            } else {
                ++it;
            }
        }

        m_lastDebounceCleanup = now;
    }

    void processEvent(const FileEvent& event) {
        FileWatcher::Filter currentFilter;
        {
            std::lock_guard<std::mutex> lock(m_filterMutex);
            currentFilter = m_filter;
        }

        // 检查事件是否通过过滤
        if (!currentFilter.passes(event)) {
            return;
        }

        // 应用防抖
        if (applyDebounce(event)) {
            return;
        }

        dispatchEvent(event);
    }

    bool applyDebounce(const FileEvent& event) {
        // 只对 MODIFY 事件应用防抖
        if (event.getType() != EventType::kModify) {
            return false;
        }

        const std::string& path = event.getPath();
        auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(m_debounceMutex);
        cleanupDebounceMap(now);

        auto it = m_debounceMap.find(path);
        if (it != m_debounceMap.end()) {
            auto lastTime = it->second;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime).count();
            if (elapsed < m_debounceTime) {
                // 更新时间戳
                it->second = now;
                return true; // 防抖生效
            }
        }

        // 添加或更新时间戳
        m_debounceMap[path] = now;
        return false;
    }

    void dispatchEvent(const FileEvent& event) {
        std::vector<EventCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(m_callbacksMutex);
            for (const auto& pair : m_watches) {
                if (isPathUnderWatch(pair.first, event.getPath(), pair.second.recursive)) {
                    callbacks.push_back(pair.second.callback);
                }
            }
        }

        for (const auto& callback : callbacks) {
            callback(event);
        }
    }
};

FileWatcher::FileWatcher(LinuxBackend backend) : m_impl(makeUnique<Impl>(backend)) {
}

FileWatcher::~FileWatcher() {
}

bool FileWatcher::addWatch(const std::string& path, bool recursive, EventCallback callback) {
    return m_impl->addWatch(path, recursive, callback);
}

bool FileWatcher::removeWatch(const std::string& path) {
    return m_impl->removeWatch(path);
}

bool FileWatcher::addWatchWithRegex(const std::string& directory, const std::string& pattern, 
                                   bool recursive, EventCallback callback) {
    return m_impl->addWatchWithRegex(directory, pattern, recursive, callback);
}

bool FileWatcher::start() {
    return m_impl->start();
}

bool FileWatcher::addTimer(Timer& timer, uint32_t timeoutMs) {
    return m_impl->addTimer(timer, timeoutMs);
}

bool FileWatcher::removeTimer(Timer& timer) {
    return m_impl->removeTimer(timer);
}

void FileWatcher::stop() {
    m_impl->stop();
}

std::error_code FileWatcher::getLastError() const {
    return m_impl->getLastError();
}

void FileWatcher::setFilter(const Filter& filter) {
    m_impl->setFilter(filter);
}

} // namespace filewatch