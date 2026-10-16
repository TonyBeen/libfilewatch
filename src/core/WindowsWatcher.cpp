#include "core/PlatformWatcher.h"
#include "utils/EncodingUtils.h"
#include "utils/PathUtils.h"
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>
#include <algorithm>
#include <unordered_map>

#ifdef _WIN32
#include <Windows.h>

namespace filewatch {

class WindowsWatcher : public PlatformWatcher {
private:
    struct WatchInfo {
        std::string path;
        bool recursive;
    };

    // 用于存储重命名事件的旧路径信息
    struct RenameInfo {
        std::string oldPath;
        PathType oldPathType;
    };

    std::vector<WatchInfo> m_watchList;
    std::atomic<bool> m_running;
    std::function<void(const FileEvent&)> m_callback;
    std::mutex m_mutex;
    RenameInfo m_renameInfo;
    std::unordered_map<std::string, PathType> m_knownPathTypes;

public:
    explicit WindowsWatcher(std::function<void(const FileEvent&)> callback)
        : m_running(false), m_callback(callback) {
    }

    ~WindowsWatcher() {
        if (m_running) {
            stop();
        }
    }

    bool addWatch(const std::string& path, bool recursive) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::string normalizedPath = utils::PathUtils::normalize(path);
        m_watchList.push_back({normalizedPath, recursive});
        m_knownPathTypes[normalizedPath] = PathType::DIRECTORY;
        return true;
    }

    bool removeWatch(const std::string& path) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::string normalizedPath = utils::PathUtils::normalize(path);
        auto it = std::find_if(m_watchList.begin(), m_watchList.end(), 
            [&normalizedPath](const WatchInfo& info) { return info.path == normalizedPath; });
        if (it != m_watchList.end()) {
            m_knownPathTypes.erase(it->path);
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
    }

private:
    std::string joinWindowsPath(const std::string& base, const std::string& child) {
        if (base.empty()) {
            return utils::PathUtils::normalize(child);
        }
        const char last = base[base.size() - 1];
        if (last == '\\' || last == '/') {
            return utils::PathUtils::normalize(base + child);
        }
        return utils::PathUtils::normalize(base + "\\" + child);
    }

    void watchLoop() {
        while (m_running) {
            std::vector<WatchInfo> watchListSnapshot;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                watchListSnapshot = m_watchList;
            }

            for (const auto& watchInfo : watchListSnapshot) {
                watchDirectory(watchInfo.path, watchInfo.recursive);
            }

            // 等待一段时间后重试
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    bool IsDirectory(const std::string& path) {
        std::wstring widePath = utils::EncodingUtils::utf8ToUtf16(path);
        if (widePath.empty()) {
            return false;
        }
        DWORD attr = GetFileAttributesW(widePath.c_str());
        return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
    }

    void watchDirectory(const std::string& path, bool recursive) {
        std::wstring widePath = utils::EncodingUtils::utf8ToUtf16(path);
        if (widePath.empty()) {
            return;
        }

        HANDLE hDir = CreateFileW(
            widePath.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            NULL
        );

        if (hDir == INVALID_HANDLE_VALUE) {
            return;
        }

        char buffer[4096];
        DWORD bytesRead;
        OVERLAPPED overlapped = {0};
        overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

        if (!overlapped.hEvent) {
            CloseHandle(hDir);
            return;
        }

        // 开始监控
        if (!ReadDirectoryChangesW(
            hDir,
            buffer,
            sizeof(buffer),
            recursive,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_LAST_WRITE,
            &bytesRead,
            &overlapped,
            NULL
        )) {
            CloseHandle(overlapped.hEvent);
            CloseHandle(hDir);
            return;
        }

        // 等待事件
        DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 100); // 100ms 超时
        if (waitResult == WAIT_OBJECT_0) {
            // 获取实际读取的字节数
            if (GetOverlappedResult(hDir, &overlapped, &bytesRead, FALSE)) {
                // 处理 Windows 目录变化事件
                DWORD offset = 0;
                FILE_NOTIFY_INFORMATION* notifyInfo;

                do {
                    notifyInfo = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer + offset);

                    // 转换文件名（UTF-16 到 UTF-8）
                    std::wstring wideName(notifyInfo->FileName, notifyInfo->FileNameLength / sizeof(wchar_t));
                    std::string fileName = utils::EncodingUtils::utf16ToUtf8(wideName);

                    std::string eventPath = joinWindowsPath(path, fileName);
                    PathType pathType = PathType::FILE;

                    // 解析事件类型
                    EventType eventType = EventType::kModify;
                    switch (notifyInfo->Action) {
                        case FILE_ACTION_ADDED:
                            eventType = EventType::kCreate;
                            pathType = IsDirectory(eventPath) ? PathType::DIRECTORY : PathType::FILE;
                            {
                                std::lock_guard<std::mutex> lock(m_mutex);
                                m_knownPathTypes[eventPath] = pathType;
                            }
                            break;
                        case FILE_ACTION_REMOVED:
                            eventType = EventType::kDelete;
                            {
                                std::lock_guard<std::mutex> lock(m_mutex);
                                std::unordered_map<std::string, PathType>::iterator it = m_knownPathTypes.find(eventPath);
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
                                std::unordered_map<std::string, PathType>::iterator it = m_knownPathTypes.find(eventPath);
                                if (it != m_knownPathTypes.end()) {
                                    pathType = it->second;
                                }
                            }
                            break;
                        case FILE_ACTION_RENAMED_OLD_NAME:
                            pathType = IsDirectory(eventPath) ? PathType::DIRECTORY : PathType::FILE;
                            {
                                std::lock_guard<std::mutex> lock(m_mutex);
                                std::unordered_map<std::string, PathType>::iterator it = m_knownPathTypes.find(eventPath);
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
                            if (pathType == PathType::DIRECTORY || IsDirectory(eventPath)) {
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
                            m_renameInfo.oldPath = "";
                            m_renameInfo.oldPathType = PathType::FILE;
                            continue;
                    }

                    FileEvent fileEvent(eventType, eventPath, pathType);

                    if (m_callback) {
                        m_callback(fileEvent);
                    }

                    offset += notifyInfo->NextEntryOffset;
                } while (notifyInfo->NextEntryOffset != 0);
            }
        }

        // 清理
        CloseHandle(overlapped.hEvent);
        CloseHandle(hDir);
    }
};

std::unique_ptr<PlatformWatcher> createWindowsWatcher(std::function<void(const FileEvent&)> callback) {
    return std::unique_ptr<PlatformWatcher>(new WindowsWatcher(callback));
}

} // namespace filewatch

#endif // _WIN32
