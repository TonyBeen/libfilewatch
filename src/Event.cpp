#include "filewatch/Event.h"

namespace filewatch {

FileEvent::FileEvent(EventType type, const std::string& path, PathType pathType,
                     const std::string& oldPath, PathType oldPathType)
    : m_type(type), m_path(path), m_pathType(pathType),
      m_oldPath(oldPath), m_oldPathType(oldPathType) {
}

EventType FileEvent::getType() const {
    return m_type;
}

std::string FileEvent::getPath() const {
    return m_path;
}

PathType FileEvent::getPathType() const {
    return m_pathType;
}

std::string FileEvent::getOldPath() const {
    return m_oldPath;
}

PathType FileEvent::getOldPathType() const {
    return m_oldPathType;
}

} // namespace filewatch