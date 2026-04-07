#include "core/PlatformWatcher.h"

namespace filewatch {

#ifdef _WIN32
std::unique_ptr<PlatformWatcher> createWindowsWatcher(std::function<void(const FileEvent&)> callback);
#elif __linux__
std::unique_ptr<PlatformWatcher> createInotifyWatcher(std::function<void(const FileEvent&)> callback);
std::unique_ptr<PlatformWatcher> createFanotifyWatcher(std::function<void(const FileEvent&)> callback);
#elif __APPLE__
std::unique_ptr<PlatformWatcher> createMacOSWatcher(std::function<void(const FileEvent&)> callback);
#endif

std::unique_ptr<PlatformWatcher> createPlatformWatcher(
    LinuxBackend backend,
    std::function<void(const FileEvent&)> callback) {
#ifdef _WIN32
    (void)backend;
    return createWindowsWatcher(callback);
#elif __linux__
    if (backend == LinuxBackend::FANOTIFY) {
        return createFanotifyWatcher(callback);
    }
    return createInotifyWatcher(callback);
#elif __APPLE__
    (void)backend;
    return createMacOSWatcher(callback);
#else
    (void)backend;
    (void)callback;
    return std::unique_ptr<PlatformWatcher>();
#endif
}

} // namespace filewatch