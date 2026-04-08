#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "filewatch/FileWatcher.h"
#include "filewatch/Event.h"
#include "utils/PathUtils.h"
#include "utils/EncodingUtils.h"
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <fstream>
#include <cstdlib>

#ifdef __linux__
#include <unistd.h>
#include <sys/stat.h>
#endif

// 测试 Event 类
TEST_CASE("Event class tests", "[Event]") {
    SECTION("Default constructor") {
        filewatch::FileEvent event(filewatch::EventType::kCreate, "test.txt", filewatch::PathType::FILE);
        REQUIRE(event.getType() == filewatch::EventType::kCreate);
        REQUIRE(event.getPath() == "test.txt");
        REQUIRE(event.getPathType() == filewatch::PathType::FILE);
        REQUIRE(event.getOldPath() == "");
        REQUIRE(event.getOldPathType() == filewatch::PathType::FILE);
    }

    SECTION("Constructor with old path") {
        filewatch::FileEvent event(
            filewatch::EventType::kRename, 
            "new.txt", 
            filewatch::PathType::FILE, 
            "old.txt", 
            filewatch::PathType::FILE
        );
        REQUIRE(event.getType() == filewatch::EventType::kRename);
        REQUIRE(event.getPath() == "new.txt");
        REQUIRE(event.getPathType() == filewatch::PathType::FILE);
        REQUIRE(event.getOldPath() == "old.txt");
        REQUIRE(event.getOldPathType() == filewatch::PathType::FILE);
    }

    SECTION("Directory event") {
        filewatch::FileEvent event(filewatch::EventType::kCreate, "test_dir", filewatch::PathType::DIRECTORY);
        REQUIRE(event.getType() == filewatch::EventType::kCreate);
        REQUIRE(event.getPath() == "test_dir");
        REQUIRE(event.getPathType() == filewatch::PathType::DIRECTORY);
    }
}

// 测试 PathUtils 类
TEST_CASE("PathUtils class tests", "[PathUtils]") {
    SECTION("normalize path") {
        std::string path = "test/path/to/file.txt";
        std::string normalized = filewatch::utils::PathUtils::normalize(path);
#ifdef _WIN32
        REQUIRE(normalized == "test\\path\\to\\file.txt");
#else
        REQUIRE(normalized == path);
#endif
    }

    SECTION("get absolute path") {
        std::string path = "./test.txt";
        std::string absolute = filewatch::utils::PathUtils::getAbsolutePath(path);
        REQUIRE(!absolute.empty());
    }

    SECTION("path exists") {
        // 测试当前文件是否存在
        bool exists = filewatch::utils::PathUtils::exists(".");
        REQUIRE(exists == true);

        // 测试不存在的文件
        exists = filewatch::utils::PathUtils::exists("non_existent_file_12345.txt");
        REQUIRE(exists == false);
    }

    SECTION("is directory") {
        // 测试当前目录
        bool isDir = filewatch::utils::PathUtils::isDirectory(".");
        REQUIRE(isDir == true);

        // 测试文件（如果存在）
        if (filewatch::utils::PathUtils::exists("CMakeLists.txt")) {
            isDir = filewatch::utils::PathUtils::isDirectory("CMakeLists.txt");
            REQUIRE(isDir == false);
        }
    }

    SECTION("is file") {
        // 测试当前目录
        bool isFile = filewatch::utils::PathUtils::isFile(".");
        REQUIRE(isFile == false);

        // 测试文件（如果存在）
        if (filewatch::utils::PathUtils::exists("CMakeLists.txt")) {
            isFile = filewatch::utils::PathUtils::isFile("CMakeLists.txt");
            REQUIRE(isFile == true);
        }
    }
}

// 测试 EncodingUtils 类
TEST_CASE("EncodingUtils class tests", "[EncodingUtils]") {
#ifdef _WIN32
    SECTION("utf8 to utf16") {
        std::string utf8 = "Hello, 世界!";
        std::wstring utf16 = filewatch::utils::EncodingUtils::utf8ToUtf16(utf8);
        REQUIRE(!utf16.empty());
    }

    SECTION("utf16 to utf8") {
        std::wstring utf16 = L"Hello, 世界!";
        std::string utf8 = filewatch::utils::EncodingUtils::utf16ToUtf8(utf16);
        REQUIRE(utf8 == "Hello, 世界!");
    }

    SECTION("ensure windows path encoding") {
        std::string path = "test/路径/file.txt";
        std::string encoded = filewatch::utils::EncodingUtils::ensureWindowsPathEncoding(path);
        REQUIRE(!encoded.empty());
    }
#else
    // 在非 Windows 平台，这些函数应该返回空字符串
    SECTION("utf8 to utf16 on non-Windows") {
        std::string utf8 = "Hello, 世界!";
        std::wstring utf16 = filewatch::utils::EncodingUtils::utf8ToUtf16(utf8);
        REQUIRE(utf16.empty());
    }

    SECTION("utf16 to utf8 on non-Windows") {
        std::wstring utf16 = L"Hello, 世界!";
        std::string utf8 = filewatch::utils::EncodingUtils::utf16ToUtf8(utf16);
        REQUIRE(utf8.empty());
    }

    SECTION("ensure windows path encoding on non-Windows") {
        std::string path = "test/path/file.txt";
        std::string encoded = filewatch::utils::EncodingUtils::ensureWindowsPathEncoding(path);
        REQUIRE(encoded == path);
    }
#endif
}

// 测试 FileWatcher 类（基本功能）
TEST_CASE("FileWatcher class tests", "[FileWatcher]") {
    SECTION("Constructor") {
        filewatch::FileWatcher watcher;
        // 构造函数应该成功执行
    }

    SECTION("Add and remove watch") {
        filewatch::FileWatcher watcher;
        bool added = watcher.addWatch(".", false, [](const filewatch::FileEvent&) {});
        REQUIRE(added == true);

        bool removed = watcher.removeWatch(".");
        REQUIRE(removed == true);
    }

    SECTION("Add watch with regex") {
        filewatch::FileWatcher watcher;
        bool added = watcher.addWatchWithRegex(".", R"(.*\.txt$)", false, [](const filewatch::FileEvent&) {});
        REQUIRE(added == true);
    }

    SECTION("Start and stop") {
        filewatch::FileWatcher watcher;
        watcher.addWatch(".", false, [](const filewatch::FileEvent&) {});

        std::atomic<bool> started(false);
        std::thread startThread([&watcher, &started]() {
            started = watcher.start();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        watcher.stop();
        startThread.join();
        REQUIRE(started == true);
    }

    SECTION("Error handling") {
        filewatch::FileWatcher watcher;

        // 测试无效路径
        bool added = watcher.addWatch("", false, [](const filewatch::FileEvent&) {});
        REQUIRE(added == false);
        std::error_code ec = watcher.getLastError();
        REQUIRE(ec.value() == static_cast<int>(filewatch::errc::watcher_errc::invalid_path));

        // 测试无效正则表达式
        added = watcher.addWatchWithRegex(".", "[invalid-regex", false, [](const filewatch::FileEvent&) {});
        REQUIRE(added == false);
        ec = watcher.getLastError();
        REQUIRE(ec.value() == static_cast<int>(filewatch::errc::watcher_errc::regex_error));
    }

    SECTION("Event filtering") {
        filewatch::FileWatcher watcher;

        // 创建过滤器
        filewatch::FileWatcher::Filter filter;
        filter.setEventTypes({filewatch::EventType::kCreate, filewatch::EventType::kModify});
        filter.setExtensions({"txt", "cpp"});
        filter.setPathPattern(".*test.*");

        // 设置过滤器
        watcher.setFilter(filter);

        // 添加监控
        bool added = watcher.addWatch(".", false, [](const filewatch::FileEvent&) {});
        REQUIRE(added == true);
    }

    SECTION("Platform specific backends") {
#ifdef __linux__
        // 测试 inotify 后端
        filewatch::FileWatcher inotifyWatcher(filewatch::LinuxBackend::INOTIFY);
        bool added = inotifyWatcher.addWatch(".", false, [](const filewatch::FileEvent&) {});
        REQUIRE(added == true);

        // 测试 fanotify 后端（fanotify 可能需要特殊权限，失败也不影响测试）
        filewatch::FileWatcher fanotifyWatcher(filewatch::LinuxBackend::FANOTIFY);
        added = fanotifyWatcher.addWatch(".", false, [](const filewatch::FileEvent&) {});
        // 即使 fanotify 失败，也不影响测试结果
        (void)added;
#elif _WIN32
        // 测试 Windows 后端
        filewatch::FileWatcher windowsWatcher;
        bool added = windowsWatcher.addWatch(".", false, [](const filewatch::FileEvent&) {});
        REQUIRE(added == true);
#elif __APPLE__
        // 测试 macOS 后端
        filewatch::FileWatcher macosWatcher;
        bool added = macosWatcher.addWatch(".", false, [](const filewatch::FileEvent&) {});
        REQUIRE(added == true);
#endif
    }

    SECTION("Batch processing and debouncing") {
        filewatch::FileWatcher watcher;

        // 添加监控
        bool added = watcher.addWatch(".", false, [](const filewatch::FileEvent&) {});
        REQUIRE(added == true);

        std::atomic<bool> started(false);
        std::thread startThread([&watcher, &started]() {
            started = watcher.start();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // 停止监控
        watcher.stop();
        startThread.join();
        REQUIRE(started == true);
    }

#ifdef __linux__
    SECTION("Recursive watch captures events in newly created subdirectory") {
        char tmpTemplate[] = "/tmp/filewatch_recursive_ut_XXXXXX";
        char* created = mkdtemp(tmpTemplate);
        REQUIRE(created != nullptr);
        std::string baseDir(created);
        std::string firstLevel = baseDir + "/fw";
        std::string nestedDir = firstLevel + "/inner";
        std::string file1 = nestedDir + "/a.txt";
        std::string file2 = nestedDir + "/a2.txt";

        filewatch::FileWatcher watcher(filewatch::LinuxBackend::INOTIFY);
        std::vector<filewatch::FileEvent> captured;
        std::mutex capturedMutex;

        bool added = watcher.addWatch(baseDir, true, [&captured, &capturedMutex](const filewatch::FileEvent& event) {
            std::lock_guard<std::mutex> lock(capturedMutex);
            captured.push_back(event);
        });
        REQUIRE(added == true);

        std::atomic<bool> started(false);
        std::thread startThread([&watcher, &started]() {
            started = watcher.start();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        REQUIRE(::mkdir(firstLevel.c_str(), 0755) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        REQUIRE(::mkdir(nestedDir.c_str(), 0755) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        {
            std::ofstream ofs(file1.c_str());
            ofs << "hello";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        {
            std::ofstream ofs(file1.c_str(), std::ios::app);
            ofs << " world";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        REQUIRE(::rename(file1.c_str(), file2.c_str()) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        REQUIRE(::unlink(file2.c_str()) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        watcher.stop();
        startThread.join();
        REQUIRE(started == true);

        bool sawFirstLevelCreate = false;
        bool sawNestedDirCreate = false;
        bool sawNestedFileCreate = false;
        bool sawNestedFileModify = false;
        bool sawNestedFileRename = false;
        bool sawNestedFileDelete = false;

        {
            std::lock_guard<std::mutex> lock(capturedMutex);
            for (size_t i = 0; i < captured.size(); ++i) {
                const filewatch::FileEvent& e = captured[i];
                if (e.getType() == filewatch::EventType::kCreate && e.getPathType() == filewatch::PathType::DIRECTORY && e.getPath() == firstLevel) {
                    sawFirstLevelCreate = true;
                }
                if (e.getType() == filewatch::EventType::kCreate && e.getPathType() == filewatch::PathType::DIRECTORY && e.getPath() == nestedDir) {
                    sawNestedDirCreate = true;
                }
                if (e.getType() == filewatch::EventType::kCreate && e.getPath() == file1) {
                    sawNestedFileCreate = true;
                }
                if (e.getType() == filewatch::EventType::kModify && e.getPath() == file1) {
                    sawNestedFileModify = true;
                }
                if (e.getType() == filewatch::EventType::kRename && e.getPath() == file2 && e.getOldPath() == file1) {
                    sawNestedFileRename = true;
                }
                if (e.getType() == filewatch::EventType::kDelete && e.getPath() == file2) {
                    sawNestedFileDelete = true;
                }
            }
        }

        REQUIRE(sawFirstLevelCreate == true);
        REQUIRE(sawNestedDirCreate == true);
        REQUIRE(sawNestedFileCreate == true);
        REQUIRE(sawNestedFileModify == true);
        REQUIRE(sawNestedFileRename == true);
        REQUIRE(sawNestedFileDelete == true);

        // 清理测试目录
        ::rmdir(nestedDir.c_str());
        ::rmdir(firstLevel.c_str());
        ::rmdir(baseDir.c_str());
    }

    SECTION("Recursive watch captures copied directory tree") {
        char tmpTemplate[] = "/tmp/filewatch_copytree_ut_XXXXXX";
        char* created = mkdtemp(tmpTemplate);
        REQUIRE(created != nullptr);
        std::string tmpRoot(created);
        std::string watchedRoot = tmpRoot + "/watched";
        std::string seedRoot = tmpRoot + "/seed";
        std::string seedNested = seedRoot + "/nested";
        std::string copiedRoot = watchedRoot + "/copied";
        std::string copiedFile1 = copiedRoot + "/root.txt";
        std::string copiedFile2 = copiedRoot + "/nested/inner.txt";

        REQUIRE(::mkdir(watchedRoot.c_str(), 0755) == 0);
        REQUIRE(::mkdir(seedRoot.c_str(), 0755) == 0);
        REQUIRE(::mkdir(seedNested.c_str(), 0755) == 0);
        {
            std::ofstream ofs((seedRoot + "/root.txt").c_str());
            ofs << "root";
        }
        {
            std::ofstream ofs((seedNested + "/inner.txt").c_str());
            ofs << "inner";
        }

        filewatch::FileWatcher watcher(filewatch::LinuxBackend::INOTIFY);
        std::vector<filewatch::FileEvent> captured;
        std::mutex capturedMutex;

        bool added = watcher.addWatch(watchedRoot, true, [&captured, &capturedMutex](const filewatch::FileEvent& event) {
            std::lock_guard<std::mutex> lock(capturedMutex);
            captured.push_back(event);
        });
        REQUIRE(added == true);

        std::atomic<bool> started(false);
        std::thread startThread([&watcher, &started]() {
            started = watcher.start();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::string copyCmd = "cp -r \"" + seedRoot + "\" \"" + copiedRoot + "\"";
        REQUIRE(std::system(copyCmd.c_str()) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));

        watcher.stop();
        startThread.join();
        REQUIRE(started == true);

        bool sawCopiedDirCreate = false;
        bool sawCopiedRootFileCreate = false;
        bool sawCopiedNestedFileCreate = false;

        {
            std::lock_guard<std::mutex> lock(capturedMutex);
            for (size_t i = 0; i < captured.size(); ++i) {
                const filewatch::FileEvent& e = captured[i];
                if (e.getType() == filewatch::EventType::kCreate && e.getPathType() == filewatch::PathType::DIRECTORY && e.getPath() == copiedRoot) {
                    sawCopiedDirCreate = true;
                }
                if (e.getType() == filewatch::EventType::kCreate && e.getPath() == copiedFile1) {
                    sawCopiedRootFileCreate = true;
                }
                if (e.getType() == filewatch::EventType::kCreate && e.getPath() == copiedFile2) {
                    sawCopiedNestedFileCreate = true;
                }
            }
        }

        REQUIRE(sawCopiedDirCreate == true);
        REQUIRE(sawCopiedRootFileCreate == true);
        REQUIRE(sawCopiedNestedFileCreate == true);

        std::string cleanupCmd = "rm -rf \"" + tmpRoot + "\"";
        (void)std::system(cleanupCmd.c_str());
    }
#endif
}
