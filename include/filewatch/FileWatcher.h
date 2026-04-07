#ifndef FILEWATCH_FILEWATCHER_H
#define FILEWATCH_FILEWATCHER_H

#include <string>
#include <functional>
#include <memory>
#include <system_error>
#include <vector>
#include <regex>
#include "Event.h"
#include "LinuxBackend.h"

namespace filewatch {

namespace errc {
/**
 * 自定义错误码
 */
enum watcher_errc {
        success = 0,              // 操作成功
        invalid_path = 1,         // 无效的路径
        path_not_exist = 2,       // 路径不存在
        permission_denied = 3,    // 权限不足
        platform_error = 4,       // 平台特定错误
        regex_error = 5,          // 正则表达式错误
        internal_error = 6,       // 内部错误
        already_watching = 7,     // 已经在监控
        watcher_not_started = 8,  // 监控器未启动
        watcher_already_running = 9, // 监控器已经在运行
        unknown_error = 10        // 未知错误
    };

    /**
     * 自定义错误类别
     */
    class watcher_error_category : public std::error_category {
    public:
        const char* name() const noexcept override;
        std::string message(int ev) const override;
    };

    /**
     * 获取错误类别单例
     */
    const watcher_error_category& watcher_category() noexcept;
}

/**
 * 文件监控器类
 */
class FileWatcher {
public:
    // 事件回调类型
    using EventCallback = std::function<void(const FileEvent&)>;

    /**
     * 构造函数
     * @param backend Linux 监控后端类型（仅 Linux 平台有效）
     */
    explicit FileWatcher(LinuxBackend backend = LinuxBackend::INOTIFY);

    /**
     * 析构函数
     */
    ~FileWatcher();

    /**
     * 添加监控路径
     * @param path 要监控的路径
     * @param recursive 是否递归监控子目录
     * @param callback 事件回调函数
     * @return 是否添加成功
     */
    bool addWatch(const std::string& path, bool recursive, EventCallback callback);

    /**
     * 移除监控路径
     * @param path 要移除监控的路径
     * @return 是否移除成功
     */
    bool removeWatch(const std::string& path);

    /**
     * 监控指定目录下的匹配正则表达式的文件
     * @param directory 要监控的目录
     * @param pattern 正则表达式模式
     * @param recursive 是否递归监控子目录
     * @param callback 事件回调函数
     * @return 是否添加成功
     */
    bool addWatchWithRegex(const std::string& directory, const std::string& pattern, 
                          bool recursive, EventCallback callback);

    /**
        * 启动监控
        * 注意：该调用会在当前线程进入事件循环，直到 stop() 被其他线程调用后才返回。
     * @return 是否启动成功
     */
    bool start();

    /**
        * 停止监控
        * 通常由其他线程调用，用于终止 start() 所在的事件循环。
     */
    void stop();

    /**
     * 获取最后一个错误
     * @return 错误码
     */
    std::error_code getLastError() const;

    /**
     * 事件过滤器类
     */
    class Filter {
    public:
        /**
         * 按事件类型过滤
         * @param types 事件类型列表
         * @return 过滤器本身（链式调用）
         */
        Filter& setEventTypes(const std::vector<EventType>& types);

        /**
         * 按文件扩展名过滤
         * @param extensions 扩展名列表（如 ".txt", ".cpp"）
         * @return 过滤器本身（链式调用）
         */
        Filter& setExtensions(const std::vector<std::string>& extensions);

        /**
         * 按路径模式过滤（正则表达式）
         * @param pattern 正则表达式模式
         * @return 过滤器本身（链式调用）
         */
        Filter& setPathPattern(const std::string& pattern);

        /**
         * 检查事件是否通过过滤
         * @param event 要检查的事件
         * @return 是否通过过滤
         */
        bool passes(const FileEvent& event) const;

    private:
        std::vector<EventType>      m_eventTypes;
        std::vector<std::string>    m_extensions;
        std::regex                  m_pathPattern;
        bool                        m_hasPathPattern = false;
    };

    /**
     * 设置过滤器
     * @param filter 过滤器对象
     */
    void setFilter(const Filter& filter);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace filewatch

// 重载 make_error_code 以便使用 std::error_code
namespace std {
    template<>
    struct is_error_code_enum<filewatch::errc::watcher_errc> : public true_type { };
}

namespace filewatch {
    std::error_code make_error_code(errc::watcher_errc e) noexcept;
}

#endif // FILEWATCH_FILEWATCHER_H