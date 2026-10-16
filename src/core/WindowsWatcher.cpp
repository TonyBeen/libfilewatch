#include "core/PlatformWatcher.h"
#include "utils/EncodingUtils.h"
#include "utils/PathUtils.h"
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>
#include <algorithm>
#include <unordered_map>
#include <memory>

#ifdef _WIN32
#include <Windows.h>

namespace filewatch {

class WindowsWatcher : public PlatformWatcher {
private:
    static const ULONG_PTR WAKEUP_KEY = 1;

    struct WatchItem {
        std::string path;
        bool recursive = false;
        HANDLE hDir = INVALID_HANDLE_VALUE;
        OVERLAPPED overlapped;
        std::vector<BYTE> buffer;
    };

    struct RenameInfo {
        std::string oldPath;
        PathType oldPathType = PathType::FILE;
    };

    HANDLE m_iocp;
    std::unordered_map<std::string, std::shared_ptr<WatchItem>> m_watches;
    std::unordered_map<std::string, PathType> m_knownPathTypes;
    RenameInfo m_renameInfo;
    std::mutex m_mutex;

    std::function<int64_t()> m_nextTimeoutProvider;
    std::function<void()> m_timerExpiryHandler;

    std::atomic<bool> m_running;
    std::function<void(const FileEvent&)> m_callback;

public:
    explicit WindowsWatcher(std::function<void(const FileEvent&)> callback)
        : m_iocp(INVALID_HANDLE_VALUE), m_running(false), m_callback(callback) {
        m_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    }

    ~WindowsWatcher() {
        if (m_running) {
            stop();
        }
        closeAllWatches();
        if (m_iocp != INVALID_HANDLE_VALUE) {
            CloseHandle(m_iocp);
            m_iocp = INVALID_HANDLE_VALUE;
        }
    }

    void wakeup() override {
        if (m_iocp != INVALID_HANDLE_VALUE) {
            PostQueuedCompletionStatus(m_iocp, 0, WAKEUP_KEY, nullptr);
        }
    }

    void setTimerHandlers(std::function<int64_t()> nextTimeoutProvider,
                          std::function<void()> expiryHandler) override {
        m_nextTimeoutProvider = std::move(nextTimeoutProvider);
        m_timerExpiryHandler = std::move(expiryHandler);
    }

    bool addWatch(const std::string& path, bool recursive) override {
        const std::string normalizedPath = utils::PathUtils::normalize(path);
        std::wstring widePath = utils::EncodingUtils::utf8ToUtf16(normalizedPath);
        if (widePath.empty()) {
            return false;
        }

        HANDLE hDir = CreateFileW(
            widePath.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr
        );

        if (hDir == INVALID_HANDLE_VALUE) {
            return false;
        }

        auto item = std::make_shared<WatchItem>();
        item->path = normalizedPath;
        item->recursive = recursive;
        item->hDir = hDir;
        item->buffer.resize(64 * 1024); // 64KB 缓冲区
        ZeroMemory(&item->overlapped, sizeof(OVERLAPPED));

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_iocp != INVALID_HANDLE_VALUE) {
            if (!CreateIoCompletionPort(hDir, m_iocp, reinterpret_cast<ULONG_PTR>(item.get()), 0)) {
                CloseHandle(hDir);
                return false;
            }
            if (!issueRead(item.get())) {
                CloseHandle(hDir);
                return false;
            }
        }

        m_watches[normalizedPath] = item;
        m_knownPathTypes[normalizedPath] = PathType::DIRECTORY;
        return true;
    }

    bool removeWatch(const std::string& path) override {
        const std::string normalizedPath = utils::PathUtils::normalize(path);
        std::shared_ptr<WatchItem> item;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_watches.find(normalizedPath);
            if (it == m_watches.end()) {
                return false;
            }
            item = it->second;
            m_watches.erase(it);
            m_knownPathTypes.erase(normalizedPath);
        }

        if (item && item->hDir != INVALID_HANDLE_VALUE) {
            CancelIoEx(item->hDir, &item->overlapped);
            CloseHandle(item->hDir);
            item->hDir = INVALID_HANDLE_VALUE;
        }
        return true;
    }

    bool start() override {
        if (m_running || m_iocp == INVALID_HANDLE_VALUE) {
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
    bool issueRead(WatchItem* item) {
        if (!item || item->hDir == INVALID_HANDLE_VALUE) {
            return false;
        }

        ZeroMemory(&item->overlapped, sizeof(OVERLAPPED));
        DWORD bytesRead = 0;
        BOOL ok = ReadDirectoryChangesW(
            item->hDir,
            item->buffer.data(),
            static_cast<DWORD>(item->buffer.size()),
            item->recursive ? TRUE : FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_ATTRIBUTES,
            &bytesRead,
            &item->overlapped,
            nullptr
        );

        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            return false;
        }
        return true;
    }

    void closeAllWatches() {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& pair : m_watches) {
            auto item = pair.second;
            if (item && item->hDir != INVALID_HANDLE_VALUE) {
                CancelIoEx(item->hDir, &item->overlapped);
                CloseHandle(item->hDir);
                item->hDir = INVALID_HANDLE_VALUE;
            }
        }
        m_watches.clear();
        m_knownPathTypes.clear();
    }

    static std::string joinWindowsPath(const std::string& base, const std::string& child) {
        if (base.empty()) {
            return utils::PathUtils::normalize(child);
        }
        const char last = base[base.size() - 1];
        if (last == '\\' || last == '/') {
            return utils::PathUtils::normalize(base + child);
        }
        return utils::PathUtils::normalize(base + "\\" + child);
    }

    static bool isDirectory(const std::string& path) {
        std::wstring widePath = utils::EncodingUtils::utf8ToUtf16(path);
        if (widePath.empty()) {
            return false;
        }
        DWORD attr = GetFileAttributesW(widePath.c_str());
        return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
    }

    void watchLoop() {
        while (m_running) {
            int64_t timeoutMs = 1000;
            if (m_nextTimeoutProvider) {
                timeoutMs = m_nextTimeoutProvider();
                if (timeoutMs < 0) timeoutMs = 0;
                if (timeoutMs > 1000) timeoutMs = 1000;
            }

            DWORD bytesTransferred = 0;
            ULONG_PTR completionKey = 0;
            LPOVERLAPPED lpOverlapped = nullptr;

            BOOL ok = GetQueuedCompletionStatus(
                m_iocp,
                &bytesTransferred,
                &completionKey,
                &lpOverlapped,
                static_cast<DWORD>(timeoutMs)
            );

            if (!ok && lpOverlapped == nullptr) {
                // 超时（WAIT_TIMEOUT）或其他非 I/O 错误，正常流转
            } else if (completionKey == WAKEUP_KEY) {
                // 由 wakeup() 触发的唤醒包，用于重算超时或退出循环
            } else if (completionKey != 0 && lpOverlapped != nullptr) {
                WatchItem* item = reinterpret_cast<WatchItem*>(completionKey);
                if (ok && bytesTransferred > 0) {
                    processNotifications(item, bytesTransferred);
                }

                // 重新下发此目录的异步监控请求
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_running && item->hDir != INVALID_HANDLE_VALUE) {
                    issueRead(item);
                }
            }

            // 统一在主线程执行到期定时器
            if (m_timerExpiryHandler) {
                m_timerExpiryHandler();
            }
        }

        // 退出后排空到期定时器
        if (m_timerExpiryHandler) {
            m_timerExpiryHandler();
        }
    }

    void processNotifications(WatchItem* item, DWORD bytesTransferred) {
        if (!item || bytesTransferred == 0) {
            return;
        }

        DWORD offset = 0;
        const BYTE* buf = item->buffer.data();

        do {
            const FILE_NOTIFY_INFORMATION* notifyInfo = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buf + offset);

            std::wstring wideName(notifyInfo->FileName, notifyInfo->FileNameLength / sizeof(wchar_t));
            std::string fileName = utils::EncodingUtils::utf16ToUtf8(wideName);
            std::string eventPath = joinWindowsPath(item->path, fileName);
            PathType pathType = PathType::FILE;

            EventType eventType = EventType::kModify;
            switch (notifyInfo->Action) {
                case FILE_ACTION_ADDED:
                    eventType = EventType::kCreate;
                    pathType = isDirectory(eventPath) ? PathType::DIRECTORY : PathType::FILE;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_knownPathTypes[eventPath] = pathType;
                    }
                    break;
                case FILE_ACTION_REMOVED:
                    eventType = EventType::kDelete;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        auto it = m_knownPathTypes.find(eventPath);
                        if (it != m_knownPathTypes.end()) {
                            pathType = it->second;
                            m_knownPathTypes.erase(it);
                        }
                    }
                    break;
                case FILE_ACTION_MODIFIED:
                    eventType = EventType::kModify;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        auto it = m_knownPathTypes.find(eventPath);
                        if (it != m_knownPathTypes.end()) {
                            pathType = it->second;
                        }
                    }
                    break;
                case FILE_ACTION_RENAMED_OLD_NAME:
                    pathType = isDirectory(eventPath) ? PathType::DIRECTORY : PathType::FILE;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        auto it = m_knownPathTypes.find(eventPath);
                        if (it != m_knownPathTypes.end()) {
                            pathType = it->second;
                            m_knownPathTypes.erase(it);
                        }
                    }
                    m_renameInfo.oldPath = eventPath;
                    m_renameInfo.oldPathType = pathType;
                    break;
                case FILE_ACTION_RENAMED_NEW_NAME:
                    eventType = EventType::kRename;
                    pathType = m_renameInfo.oldPathType;
                    if (pathType == PathType::DIRECTORY || isDirectory(eventPath)) {
                        pathType = PathType::DIRECTORY;
                    }
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_knownPathTypes[eventPath] = pathType;
                    }
                    if (m_callback) {
                        FileEvent fileEvent(eventType, eventPath, pathType, m_renameInfo.oldPath, m_renameInfo.oldPathType);
                        m_callback(fileEvent);
                    }
                    m_renameInfo.oldPath.clear();
                    m_renameInfo.oldPathType = PathType::FILE;
                    goto next_entry;
                default:
                    break;
            }

            if (m_callback) {
                FileEvent fileEvent(eventType, eventPath, pathType);
                m_callback(fileEvent);
            }

        next_entry:
            if (notifyInfo->NextEntryOffset == 0) {
                break;
            }
            offset += notifyInfo->NextEntryOffset;
        } while (offset < bytesTransferred);
    }
};

std::unique_ptr<PlatformWatcher> createWindowsWatcher(std::function<void(const FileEvent&)> callback) {
    return std::unique_ptr<PlatformWatcher>(new WindowsWatcher(callback));
}

} // namespace filewatch

#endif // _WIN32
