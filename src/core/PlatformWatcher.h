#ifndef PLATFORM_WATCHER_H
#define PLATFORM_WATCHER_H

#include <memory>
#include <string>
#include <functional>
#include "filewatch/Event.h"
#include "filewatch/LinuxBackend.h"

namespace filewatch {

class PlatformWatcher {
public:
    virtual bool addWatch(const std::string& path, bool recursive) = 0;
    virtual bool removeWatch(const std::string& path) = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual ~PlatformWatcher() = default;
};

std::unique_ptr<PlatformWatcher> createPlatformWatcher(
    LinuxBackend backend,
    std::function<void(const FileEvent&)> callback);

} // namespace filewatch

#endif // PLATFORM_WATCHER_H
