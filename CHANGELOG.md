# Changelog

本项目的所有重要更改将记录在此文件中。

格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/)，
版本号遵循 [语义化版本 (Semantic Versioning)](https://semver.org/lang/zh-CN/)。

## [0.2.5] - 2026-10-19

### Added (新增)
- **多平台高性能事件驱动架构**：
  - **Linux**：将事件循环驱动全面升级为基于 `epoll` (`epoll_wait` + `eventfd`) 的现代化架构。
  - **Windows**：重构为基于完成端口 (`IOCP`，`GetQueuedCompletionStatus`) 的异步驱动架构，支持真正的异步 I/O 与即时唤醒。
  - **macOS**：重构为以 `kqueue` (`kevent`) 为单一核心的主事件循环，彻底摆脱 `select` 的 1024 描述符限制，零额外后台线程。
- **定时器管理机制 (Timer)**：
  - 引入零动态内存堆分配（外部对象持有）的定时器机制（参考 libevent 设计风格）。
  - 支持单次与循环定时器，在无活跃定时器时智能阻塞 1 秒，有事件时即时唤醒响应。
  - 所有文件事件回调与定时器回调均在调用 `FileWatcher::start()` 的主线程统一串行执行，为上层业务提供无锁调用保证。
- **黑名单路径与正则表达式过滤 (Filter)**：
  - 扩展 `FileWatcher::Filter` 类，新增 `addExcludePattern` 与 `setExcludePatterns` 接口，支持通过正则表达式排除深层或编译时动态生成的目录（如 `build-*`、`.git`、`CMakeFiles` 等）。
  - 新增 `addExcludePath` 与 `setExcludePaths` 接口，支持按固定路径前缀高效过滤指定子目录。
  - 过滤规则在事件分发前置第一阶段执行，被拦截事件立即丢弃，零堆内存分配、不占用防抖映射表。
- **版本头文件 (Version.h)**：
  - 新增由 CMake `configure_file` 动态生成的 `<filewatch/Version.h>` 头文件。
  - 提供 `LIBFILEWATCH_VERSION_MAJOR`、`LIBFILEWATCH_VERSION_MINOR`、`LIBFILEWATCH_VERSION_PATCH`、`LIBFILEWATCH_VERSION_STRING` 宏定义以及 `filewatch::Version` 命名空间静态方法。
- **跨平台持续集成 (CI)**：
  - 新增 GitHub Actions 跨平台 CI 工作流 (`.github/workflows/ci.yml`)，覆盖 Ubuntu (GCC)、macOS (Clang) 与 Windows (MSVC) 的编译与全量单元测试自动化。
- **单元测试矩阵完善**：
  - 单元测试扩展至 225 项断言，覆盖多目录隔离、防抖清理、定时器调度与状态转移、级联注销以及运行时动态目录排除等核心场景。

### Changed (变更)
- **Linux inotify 递归监控级联注销**：
  - 引入 `RootWatch` 树形关联追踪结构；
  - 调用 `removeWatch(rootPath)` 时，自动级联注销该根目录在内核中分配的所有子目录 watch 描述符 (`wd`)，彻底解决递归监控下子目录内核描述符泄漏的问题；
  - 动态创建/移动进来的子目录自动继承父级根监控的关联从属；
  - 增加对物理删除目录产生的 `IN_IGNORED` 事件的主动捕获与状态清理。

### Fixed (修复)
- 修复 Windows (MSVC) 下防抖时间计算时，64 位整型转换为 `long` 导致的截断与编译警告 (`C4244`)。
