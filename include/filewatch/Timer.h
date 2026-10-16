#ifndef FILEWATCH_TIMER_H
#define FILEWATCH_TIMER_H

#include <functional>
#include <chrono>
#include <cstdint>

namespace filewatch {

class FileWatcher;

/**
 * 定时器类（参考 libevent 模式，外部持有对象，无需重复动态分配内存）
 */
class Timer {
public:
    using Callback = std::function<void(Timer&)>;
    using SimpleCallback = std::function<void()>;

    Timer() = default;

    explicit Timer(Callback callback, bool repeating = false)
        : m_callback(std::move(callback)), m_repeating(repeating) {}

    explicit Timer(SimpleCallback simpleCallback, bool repeating = false)
        : m_repeating(repeating) {
        if (simpleCallback) {
            m_callback = [simpleCallback](Timer&) { simpleCallback(); };
        }
    }

    ~Timer() {
        m_active = false;
    }

    // 禁止拷贝，保证由调用者持有的唯一定时器实体
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    // 允许移动
    Timer(Timer&& other) noexcept
        : m_callback(std::move(other.m_callback)),
          m_repeating(other.m_repeating),
          m_active(other.m_active),
          m_intervalMs(other.m_intervalMs),
          m_expireTime(other.m_expireTime),
          m_id(other.m_id) {
        other.m_active = false;
    }

    Timer& operator=(Timer&& other) noexcept {
        if (this != &other) {
            m_callback = std::move(other.m_callback);
            m_repeating = other.m_repeating;
            m_active = other.m_active;
            m_intervalMs = other.m_intervalMs;
            m_expireTime = other.m_expireTime;
            m_id = other.m_id;
            other.m_active = false;
        }
        return *this;
    }

    void setCallback(Callback callback) {
        m_callback = std::move(callback);
    }

    void setCallback(SimpleCallback simpleCallback) {
        if (simpleCallback) {
            m_callback = [simpleCallback](Timer&) { simpleCallback(); };
        } else {
            m_callback = nullptr;
        }
    }

    void setRepeating(bool repeating) {
        m_repeating = repeating;
    }

    bool isRepeating() const {
        return m_repeating;
    }

    bool isActive() const {
        return m_active;
    }

    uint32_t getIntervalMs() const {
        return m_intervalMs;
    }

private:
    friend class FileWatcher;

    Callback m_callback;
    bool m_repeating = false;
    bool m_active = false;
    uint32_t m_intervalMs = 0;
    std::chrono::steady_clock::time_point m_expireTime;
    uint64_t m_id = 0;
};

} // namespace filewatch

#endif // FILEWATCH_TIMER_H
