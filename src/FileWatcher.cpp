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

bool filewatch::FileWatcher::Filter::passes(const FileEvent& event) const {
    // 检查事件类型
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

    // 检查文件扩展名
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

    // 检查路径模式
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
}

std::error_code make_error_code(errc::watcher_errc e) noexcept {
    return std::error_code(static_cast<int>(e), errc::watcher_category());
}



class FileWatcher::Impl {
public:
    Impl(LinuxBackend backend) : m_lastError(make_error_code(errc::watcher_errc::success)),
                                m_debounceTime(50) {
        m_platformWatcher = createPlatformWatcher(backend, [this](const FileEvent& event) {
            processEvent(event);
        });
    }

    ~Impl() {
    }

    bool addWatch(const std::string& path, bool recursive, EventCallback callback) {
        if (path.empty()) {
            setError(errc::watcher_errc::invalid_path);
            return false;
        }

        if (!utils::PathUtils::exists(path)) {
            setError(errc::watcher_errc::path_not_exist);
            return false;
        }

        try {
            {
                std::lock_guard<std::mutex> lock(m_callbacksMutex);
                if (m_callbacks.find(path) != m_callbacks.end()) {
                    setError(errc::watcher_errc::already_watching);
                    return false;
                }
                m_callbacks[path] = callback;
            }

            if (!m_platformWatcher->addWatch(path, recursive)) {
                setError(errc::watcher_errc::platform_error);
                std::lock_guard<std::mutex> lock(m_callbacksMutex);
                m_callbacks.erase(path);
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

        try {
            {
                std::lock_guard<std::mutex> lock(m_callbacksMutex);
                m_callbacks.erase(path);
            }
            if (!m_platformWatcher->removeWatch(path)) {
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

    void setFilter(const FileWatcher::Filter& filter) {
        std::lock_guard<std::mutex> lock(m_filterMutex);
        m_filter = filter;
    }

private:
    static bool isPathUnderWatch(const std::string& watchPath, const std::string& eventPath) {
        if (watchPath.empty()) {
            return false;
        }

        std::string w = watchPath;
        std::string e = eventPath;
        std::replace(w.begin(), w.end(), '\\', '/');
        std::replace(e.begin(), e.end(), '\\', '/');

        if (e == w) {
            return true;
        }
        if (e.size() <= w.size() || e.compare(0, w.size(), w) != 0) {
            return false;
        }
        if (w[w.size() - 1] == '/') {
            return true;
        }
        return e[w.size()] == '/';
    }

    std::unique_ptr<PlatformWatcher> m_platformWatcher;
    std::unordered_map<std::string, EventCallback> m_callbacks;
    mutable std::mutex m_callbacksMutex;
    std::error_code m_lastError; // 存储最后一个错误
    mutable std::mutex m_errorMutex;
    FileWatcher::Filter m_filter; // 事件过滤器
    mutable std::mutex m_filterMutex;

    // 仅保留防抖，不再使用后台批处理线程
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_debounceMap; // 防抖映射
    std::mutex m_debounceMutex; // 防抖互斥锁
    int m_debounceTime; // 防抖时间（毫秒）

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
        if (event.getType() != EventType::MODIFY) {
            return false;
        }

        const std::string& path = event.getPath();
        auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(m_debounceMutex);
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
            for (const auto& pair : m_callbacks) {
                if (isPathUnderWatch(pair.first, event.getPath())) {
                    callbacks.push_back(pair.second);
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