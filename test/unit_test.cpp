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

#if defined(__linux__) || defined(__APPLE__)
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

        // 测试不存在的路径
        REQUIRE(filewatch::utils::PathUtils::isDirectory("non_existent_folder_abc") == false);
        REQUIRE(filewatch::utils::PathUtils::isDirectory("") == false);

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

        // 测试不存在的路径与空路径
        REQUIRE(filewatch::utils::PathUtils::isFile("non_existent_file_abc") == false);
        REQUIRE(filewatch::utils::PathUtils::isFile("") == false);

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

        // 测试不存在的路径
        added = watcher.addWatch("non_existent_directory_987654321", false, [](const filewatch::FileEvent&) {});
        REQUIRE(added == false);
        ec = watcher.getLastError();
        REQUIRE(ec.value() == static_cast<int>(filewatch::errc::watcher_errc::path_not_exist));

        // 测试重复添加路径
        added = watcher.addWatch(".", false, [](const filewatch::FileEvent&) {});
        REQUIRE(added == true);
        added = watcher.addWatch(".", false, [](const filewatch::FileEvent&) {});
        REQUIRE(added == false);
        ec = watcher.getLastError();
        REQUIRE(ec.value() == static_cast<int>(filewatch::errc::watcher_errc::already_watching));
        watcher.removeWatch(".");

        // 测试移除空路径
        bool removed = watcher.removeWatch("");
        REQUIRE(removed == false);
        ec = watcher.getLastError();
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
        filter.setExtensions({".txt", ".cpp"});
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

#if defined(__linux__) || defined(__APPLE__)
    SECTION("Recursive watch captures events in newly created subdirectory") {
        char tmpTemplate[] = "/tmp/filewatch_recursive_ut_XXXXXX";
        char* created = mkdtemp(tmpTemplate);
        REQUIRE(created != nullptr);
        std::string baseDir = filewatch::utils::PathUtils::getAbsolutePath(created);
        std::string firstLevel = baseDir + "/fw";
        std::string nestedDir = firstLevel + "/inner";
        std::string file1 = nestedDir + "/a.txt";
        std::string file2 = nestedDir + "/a2.txt";

        filewatch::FileWatcher watcher;
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
        std::string tmpRoot = filewatch::utils::PathUtils::getAbsolutePath(created);
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

        filewatch::FileWatcher watcher;
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

    SECTION("Single file watch") {
        char tmpTemplate[] = "/tmp/filewatch_single_ut_XXXXXX";
        char* created = mkdtemp(tmpTemplate);
        REQUIRE(created != nullptr);
        std::string baseDir = filewatch::utils::PathUtils::getAbsolutePath(created);
        std::string targetFile = baseDir + "/target.txt";

        // 先创建文件
        {
            std::ofstream ofs(targetFile.c_str());
            ofs << "initial";
        }

        filewatch::FileWatcher watcher;
        std::vector<filewatch::FileEvent> captured;
        std::mutex capturedMutex;

        bool added = watcher.addWatch(targetFile, false, [&captured, &capturedMutex](const filewatch::FileEvent& event) {
            std::lock_guard<std::mutex> lock(capturedMutex);
            captured.push_back(event);
        });
        REQUIRE(added == true);

        std::atomic<bool> started(false);
        std::thread startThread([&watcher, &started]() {
            started = watcher.start();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // 修改文件
        {
            std::ofstream ofs(targetFile.c_str(), std::ios::app);
            ofs << " appended content";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // 删除文件
        REQUIRE(::unlink(targetFile.c_str()) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        watcher.stop();
        startThread.join();
        REQUIRE(started == true);

        bool sawModify = false;
        bool sawDelete = false;
        {
            std::lock_guard<std::mutex> lock(capturedMutex);
            for (const auto& e : captured) {
                if (e.getType() == filewatch::EventType::kModify && e.getPath() == targetFile) {
                    sawModify = true;
                }
                if (e.getType() == filewatch::EventType::kDelete && e.getPath() == targetFile) {
                    sawDelete = true;
                }
            }
        }
        REQUIRE(sawModify == true);
        REQUIRE(sawDelete == true);

        ::rmdir(baseDir.c_str());
    }

    SECTION("Event filter in runtime FileWatcher") {
        char tmpTemplate[] = "/tmp/filewatch_filter_ut_XXXXXX";
        char* created = mkdtemp(tmpTemplate);
        REQUIRE(created != nullptr);
        std::string baseDir = filewatch::utils::PathUtils::getAbsolutePath(created);
        std::string logFile = baseDir + "/app.log";
        std::string tmpFile = baseDir + "/app.tmp";

        filewatch::FileWatcher watcher;
        filewatch::FileWatcher::Filter filter;
        filter.setExtensions({".log"});
        watcher.setFilter(filter);

        std::vector<filewatch::FileEvent> captured;
        std::mutex capturedMutex;

        bool added = watcher.addWatch(baseDir, false, [&captured, &capturedMutex](const filewatch::FileEvent& event) {
            std::lock_guard<std::mutex> lock(capturedMutex);
            captured.push_back(event);
        });
        REQUIRE(added == true);

        std::atomic<bool> started(false);
        std::thread startThread([&watcher, &started]() {
            started = watcher.start();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        {
            std::ofstream ofs(logFile.c_str());
            ofs << "log entry";
        }
        {
            std::ofstream ofs(tmpFile.c_str());
            ofs << "temp entry";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        watcher.stop();
        startThread.join();
        REQUIRE(started == true);

        bool sawLogFile = false;
        bool sawTmpFile = false;
        {
            std::lock_guard<std::mutex> lock(capturedMutex);
            for (const auto& e : captured) {
                if (e.getPath() == logFile) {
                    sawLogFile = true;
                }
                if (e.getPath() == tmpFile) {
                    sawTmpFile = true;
                }
            }
        }
        REQUIRE(sawLogFile == true);
        REQUIRE(sawTmpFile == false);

        ::unlink(logFile.c_str());
        ::unlink(tmpFile.c_str());
        ::rmdir(baseDir.c_str());
    }

    SECTION("Debounce modifies in runtime") {
        char tmpTemplate[] = "/tmp/filewatch_debounce_ut_XXXXXX";
        char* created = mkdtemp(tmpTemplate);
        REQUIRE(created != nullptr);
        std::string baseDir = filewatch::utils::PathUtils::getAbsolutePath(created);
        std::string testFile = baseDir + "/burst.txt";

        {
            std::ofstream ofs(testFile.c_str());
            ofs << "init";
        }

        filewatch::FileWatcher watcher;
        std::atomic<int> modifyCount(0);

        bool added = watcher.addWatch(baseDir, false, [&modifyCount, &testFile](const filewatch::FileEvent& event) {
            if (event.getType() == filewatch::EventType::kModify && event.getPath() == testFile) {
                ++modifyCount;
            }
        });
        REQUIRE(added == true);

        std::atomic<bool> started(false);
        std::thread startThread([&watcher, &started]() {
            started = watcher.start();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // 连续快速追加写入 8 次（时间间隔极短，< 50ms debounce 窗口）
        for (int i = 0; i < 8; ++i) {
            std::ofstream ofs(testFile.c_str(), std::ios::app);
            ofs << "x";
            ofs.flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        watcher.stop();
        startThread.join();
        REQUIRE(started == true);

        // 防抖生效后，收到的 modify 事件应该明显少于写入次数
        REQUIRE(modifyCount.load() >= 1);
        REQUIRE(modifyCount.load() < 8);

        ::unlink(testFile.c_str());
        ::rmdir(baseDir.c_str());
    }

    SECTION("Non-recursive directory watch does not capture events in subdirectories") {
        char tmpTemplate[] = "/tmp/filewatch_nonrec_ut_XXXXXX";
        char* created = mkdtemp(tmpTemplate);
        REQUIRE(created != nullptr);
        std::string baseDir = filewatch::utils::PathUtils::getAbsolutePath(created);
        std::string subDir = baseDir + "/subdir";
        std::string rootFile = baseDir + "/root.txt";
        std::string subFile = subDir + "/inner.txt";

        REQUIRE(::mkdir(subDir.c_str(), 0755) == 0);

        filewatch::FileWatcher watcher;
        std::vector<filewatch::FileEvent> captured;
        std::mutex capturedMutex;

        // recursive = false
        bool added = watcher.addWatch(baseDir, false, [&captured, &capturedMutex](const filewatch::FileEvent& event) {
            std::lock_guard<std::mutex> lock(capturedMutex);
            captured.push_back(event);
        });
        REQUIRE(added == true);

        std::atomic<bool> started(false);
        std::thread startThread([&watcher, &started]() {
            started = watcher.start();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        {
            std::ofstream ofs(rootFile.c_str());
            ofs << "root";
        }
        {
            std::ofstream ofs(subFile.c_str());
            ofs << "sub";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        watcher.stop();
        startThread.join();
        REQUIRE(started == true);

        bool sawRootFile = false;
        bool sawSubFile = false;
        {
            std::lock_guard<std::mutex> lock(capturedMutex);
            for (const auto& e : captured) {
                if (e.getPath() == rootFile) {
                    sawRootFile = true;
                }
                if (e.getPath() == subFile) {
                    sawSubFile = true;
                }
            }
        }

        REQUIRE(sawRootFile == true);
        REQUIRE(sawSubFile == false);

        ::unlink(rootFile.c_str());
        ::unlink(subFile.c_str());
        ::rmdir(subDir.c_str());
        ::rmdir(baseDir.c_str());
    }

    SECTION("addWatchWithRegex filters events in runtime") {
        char tmpTemplate[] = "/tmp/filewatch_regex_ut_XXXXXX";
        char* created = mkdtemp(tmpTemplate);
        REQUIRE(created != nullptr);
        std::string baseDir = filewatch::utils::PathUtils::getAbsolutePath(created);
        std::string matchFile = baseDir + "/important.log";
        std::string ignoreFile = baseDir + "/ignore.txt";

        filewatch::FileWatcher watcher;
        std::vector<filewatch::FileEvent> captured;
        std::mutex capturedMutex;

        bool added = watcher.addWatchWithRegex(baseDir, R"(.*\.log$)", false, [&captured, &capturedMutex](const filewatch::FileEvent& event) {
            std::lock_guard<std::mutex> lock(capturedMutex);
            captured.push_back(event);
        });
        REQUIRE(added == true);

        std::atomic<bool> started(false);
        std::thread startThread([&watcher, &started]() {
            started = watcher.start();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        {
            std::ofstream ofs(matchFile.c_str());
            ofs << "match";
        }
        {
            std::ofstream ofs(ignoreFile.c_str());
            ofs << "ignore";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        watcher.stop();
        startThread.join();
        REQUIRE(started == true);

        bool sawMatch = false;
        bool sawIgnore = false;
        {
            std::lock_guard<std::mutex> lock(capturedMutex);
            for (const auto& e : captured) {
                if (e.getPath() == matchFile) {
                    sawMatch = true;
                }
                if (e.getPath() == ignoreFile) {
                    sawIgnore = true;
                }
            }
        }

        REQUIRE(sawMatch == true);
        REQUIRE(sawIgnore == false);

        ::unlink(matchFile.c_str());
        ::unlink(ignoreFile.c_str());
        ::rmdir(baseDir.c_str());
    }

    SECTION("removeWatch stops delivering events in runtime") {
        char tmpTemplate[] = "/tmp/filewatch_rmwatch_ut_XXXXXX";
        char* created = mkdtemp(tmpTemplate);
        REQUIRE(created != nullptr);
        std::string baseDir = filewatch::utils::PathUtils::getAbsolutePath(created);
        std::string fileBefore = baseDir + "/before.txt";
        std::string fileAfter = baseDir + "/after.txt";

        filewatch::FileWatcher watcher;
        std::vector<filewatch::FileEvent> captured;
        std::mutex capturedMutex;

        bool added = watcher.addWatch(baseDir, false, [&captured, &capturedMutex](const filewatch::FileEvent& event) {
            std::lock_guard<std::mutex> lock(capturedMutex);
            captured.push_back(event);
        });
        REQUIRE(added == true);

        std::atomic<bool> started(false);
        std::thread startThread([&watcher, &started]() {
            started = watcher.start();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // 移除前创建文件，应该被捕获
        {
            std::ofstream ofs(fileBefore.c_str());
            ofs << "before";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // 移除监控
        REQUIRE(watcher.removeWatch(baseDir) == true);

        // 移除后创建文件，不应该被捕获
        {
            std::ofstream ofs(fileAfter.c_str());
            ofs << "after";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        watcher.stop();
        startThread.join();
        REQUIRE(started == true);

        bool sawBefore = false;
        bool sawAfter = false;
        {
            std::lock_guard<std::mutex> lock(capturedMutex);
            for (const auto& e : captured) {
                if (e.getPath() == fileBefore) {
                    sawBefore = true;
                }
                if (e.getPath() == fileAfter) {
                    sawAfter = true;
                }
            }
        }

        REQUIRE(sawBefore == true);
        REQUIRE(sawAfter == false);

        ::unlink(fileBefore.c_str());
        ::unlink(fileAfter.c_str());
        ::rmdir(baseDir.c_str());
    }

    SECTION("removeWatch cascades to subdirectories for recursive watches") {
        char tmpTemplate[] = "/tmp/filewatch_rmcascade_ut_XXXXXX";
        char* created = mkdtemp(tmpTemplate);
        REQUIRE(created != nullptr);
        std::string baseDir = filewatch::utils::PathUtils::getAbsolutePath(created);
        std::string subDir = baseDir + "/subdir";
        REQUIRE(mkdir(subDir.c_str(), 0755) == 0);

        std::string subFileBefore = subDir + "/before.txt";
        std::string subFileAfter = subDir + "/after.txt";

        filewatch::FileWatcher watcher;
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

        // 移除前在子目录创建文件，应该被捕获
        {
            std::ofstream ofs(subFileBefore.c_str());
            ofs << "sub_before";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // 移除递归监控根目录
        REQUIRE(watcher.removeWatch(baseDir) == true);

        // 移除后在子目录创建文件，不应该被捕获（子目录的 watch 已被级联移除）
        {
            std::ofstream ofs(subFileAfter.c_str());
            ofs << "sub_after";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        watcher.stop();
        startThread.join();
        REQUIRE(started == true);

        bool sawBefore = false;
        bool sawAfter = false;
        {
            std::lock_guard<std::mutex> lock(capturedMutex);
            for (const auto& e : captured) {
                if (e.getPath() == subFileBefore) {
                    sawBefore = true;
                }
                if (e.getPath() == subFileAfter) {
                    sawAfter = true;
                }
            }
        }

        REQUIRE(sawBefore == true);
        REQUIRE(sawAfter == false);

        ::unlink(subFileBefore.c_str());
        ::unlink(subFileAfter.c_str());
        ::rmdir(subDir.c_str());
        ::rmdir(baseDir.c_str());
    }

    SECTION("Dynamic build directories exclusion in runtime") {
        char tmpTemplate[] = "/tmp/filewatch_rmexclude_ut_XXXXXX";
        char* created = mkdtemp(tmpTemplate);
        REQUIRE(created != nullptr);
        std::string baseDir = filewatch::utils::PathUtils::getAbsolutePath(created);
        std::string srcDir = baseDir + "/src";
        REQUIRE(mkdir(srcDir.c_str(), 0755) == 0);

        filewatch::FileWatcher watcher;
        filewatch::FileWatcher::Filter filter;
        // 动态排除以 build- 开头的任何目录
        filter.addExcludePattern(R"((^|[/\\])build-[^/\\]+([/\\]|$))");
        watcher.setFilter(filter);

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

        // 动态创建编译目录 build-debug 并在其中生成目标文件
        std::string buildDebugDir = baseDir + "/build-debug";
        REQUIRE(mkdir(buildDebugDir.c_str(), 0755) == 0);
        std::string objFile = buildDebugDir + "/main.o";
        {
            std::ofstream ofs(objFile.c_str());
            ofs << "binary data";
        }

        // 正常业务文件
        std::string srcFile = srcDir + "/main.cpp";
        {
            std::ofstream ofs(srcFile.c_str());
            ofs << "int main() {}";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        watcher.stop();
        startThread.join();
        REQUIRE(started == true);

        bool sawSrc = false;
        bool sawBuildDebug = false;
        {
            std::lock_guard<std::mutex> lock(capturedMutex);
            for (const auto& e : captured) {
                if (e.getPath() == srcFile) {
                    sawSrc = true;
                }
                if (e.getPath().find("build-debug") != std::string::npos) {
                    sawBuildDebug = true;
                }
            }
        }

        REQUIRE(sawSrc == true);
        REQUIRE(sawBuildDebug == false);

        ::unlink(objFile.c_str());
        ::rmdir(buildDebugDir.c_str());
        ::unlink(srcFile.c_str());
        ::rmdir(srcDir.c_str());
        ::rmdir(baseDir.c_str());
    }

    SECTION("Multiple watched directories callback isolation") {
        char tmpTemplateA[] = "/tmp/filewatch_multia_ut_XXXXXX";
        char tmpTemplateB[] = "/tmp/filewatch_multib_ut_XXXXXX";
        char* createdA = mkdtemp(tmpTemplateA);
        char* createdB = mkdtemp(tmpTemplateB);
        REQUIRE(createdA != nullptr);
        REQUIRE(createdB != nullptr);
        std::string baseDirA = filewatch::utils::PathUtils::getAbsolutePath(createdA);
        std::string baseDirB = filewatch::utils::PathUtils::getAbsolutePath(createdB);
        std::string fileA = baseDirA + "/file_a.txt";
        std::string fileB = baseDirB + "/file_b.txt";

        filewatch::FileWatcher watcher;
        std::vector<std::string> capturedA;
        std::vector<std::string> capturedB;
        std::mutex mutexA;
        std::mutex mutexB;

        bool addedA = watcher.addWatch(baseDirA, false, [&capturedA, &mutexA](const filewatch::FileEvent& event) {
            std::lock_guard<std::mutex> lock(mutexA);
            capturedA.push_back(event.getPath());
        });
        bool addedB = watcher.addWatch(baseDirB, false, [&capturedB, &mutexB](const filewatch::FileEvent& event) {
            std::lock_guard<std::mutex> lock(mutexB);
            capturedB.push_back(event.getPath());
        });
        REQUIRE(addedA == true);
        REQUIRE(addedB == true);

        std::atomic<bool> started(false);
        std::thread startThread([&watcher, &started]() {
            started = watcher.start();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        {
            std::ofstream ofs(fileA.c_str());
            ofs << "data a";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        {
            std::ofstream ofs(fileB.c_str());
            ofs << "data b";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        watcher.stop();
        startThread.join();
        REQUIRE(started == true);

        bool aSawA = false;
        bool aSawB = false;
        {
            std::lock_guard<std::mutex> lock(mutexA);
            for (const auto& p : capturedA) {
                if (p == fileA) aSawA = true;
                if (p == fileB) aSawB = true;
            }
        }

        bool bSawA = false;
        bool bSawB = false;
        {
            std::lock_guard<std::mutex> lock(mutexB);
            for (const auto& p : capturedB) {
                if (p == fileA) bSawA = true;
                if (p == fileB) bSawB = true;
            }
        }

        REQUIRE(aSawA == true);
        REQUIRE(aSawB == false);
        REQUIRE(bSawA == false);
        REQUIRE(bSawB == true);

        ::unlink(fileA.c_str());
        ::unlink(fileB.c_str());
        ::rmdir(baseDirA.c_str());
        ::rmdir(baseDirB.c_str());
    }

    SECTION("Directory rename captures rename event and old path") {
        char tmpTemplate[] = "/tmp/filewatch_dirrename_ut_XXXXXX";
        char* created = mkdtemp(tmpTemplate);
        REQUIRE(created != nullptr);
        std::string baseDir = filewatch::utils::PathUtils::getAbsolutePath(created);
        std::string oldSubDir = baseDir + "/dir_old";
        std::string newSubDir = baseDir + "/dir_new";

        filewatch::FileWatcher watcher;
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

        REQUIRE(::mkdir(oldSubDir.c_str(), 0755) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        REQUIRE(::rename(oldSubDir.c_str(), newSubDir.c_str()) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        watcher.stop();
        startThread.join();
        REQUIRE(started == true);

        bool sawDirRename = false;
        {
            std::lock_guard<std::mutex> lock(capturedMutex);
            for (const auto& e : captured) {
                if (e.getType() == filewatch::EventType::kRename &&
                    e.getPathType() == filewatch::PathType::DIRECTORY &&
                    e.getPath() == newSubDir &&
                    e.getOldPath() == oldSubDir) {
                    sawDirRename = true;
                }
            }
        }

        REQUIRE(sawDirRename == true);

        ::rmdir(newSubDir.c_str());
        ::rmdir(baseDir.c_str());
    }
#endif
}

// 测试 FileWatcher::Filter 类
TEST_CASE("Filter class tests", "[Filter]") {
    SECTION("Default filter passes everything") {
        filewatch::FileWatcher::Filter filter;
        filewatch::FileEvent fileEv(filewatch::EventType::kCreate, "/path/to/test.txt", filewatch::PathType::FILE);
        filewatch::FileEvent dirEv(filewatch::EventType::kDelete, "/path/to/dir", filewatch::PathType::DIRECTORY);
        REQUIRE(filter.passes(fileEv) == true);
        REQUIRE(filter.passes(dirEv) == true);
    }

    SECTION("Event types filtering") {
        filewatch::FileWatcher::Filter filter;
        filter.setEventTypes({filewatch::EventType::kModify, filewatch::EventType::kDelete});

        filewatch::FileEvent evCreate(filewatch::EventType::kCreate, "/test.txt", filewatch::PathType::FILE);
        filewatch::FileEvent evModify(filewatch::EventType::kModify, "/test.txt", filewatch::PathType::FILE);
        filewatch::FileEvent evDelete(filewatch::EventType::kDelete, "/test.txt", filewatch::PathType::FILE);
        filewatch::FileEvent evRename(filewatch::EventType::kRename, "/new.txt", filewatch::PathType::FILE, "/old.txt");

        REQUIRE(filter.passes(evCreate) == false);
        REQUIRE(filter.passes(evModify) == true);
        REQUIRE(filter.passes(evDelete) == true);
        REQUIRE(filter.passes(evRename) == false);
    }

    SECTION("Extensions filtering") {
        filewatch::FileWatcher::Filter filter;
        filter.setExtensions({".txt", ".log"});

        filewatch::FileEvent evTxt(filewatch::EventType::kCreate, "file.txt", filewatch::PathType::FILE);
        filewatch::FileEvent evLog(filewatch::EventType::kModify, "/path/to/file.log", filewatch::PathType::FILE);
        filewatch::FileEvent evCpp(filewatch::EventType::kCreate, "file.cpp", filewatch::PathType::FILE);
        filewatch::FileEvent evNoExt(filewatch::EventType::kCreate, "file_without_ext", filewatch::PathType::FILE);
        // 目录不受文件扩展名限制
        filewatch::FileEvent evDir(filewatch::EventType::kCreate, "/path/to/folder.txt", filewatch::PathType::DIRECTORY);

        REQUIRE(filter.passes(evTxt) == true);
        REQUIRE(filter.passes(evLog) == true);
        REQUIRE(filter.passes(evCpp) == false);
        REQUIRE(filter.passes(evNoExt) == false);
        REQUIRE(filter.passes(evDir) == true);
    }

    SECTION("Path pattern filtering") {
        filewatch::FileWatcher::Filter filter;
        filter.setPathPattern(".*\\.cpp$");

        filewatch::FileEvent evCpp(filewatch::EventType::kCreate, "/src/main.cpp", filewatch::PathType::FILE);
        filewatch::FileEvent evH(filewatch::EventType::kCreate, "/src/main.h", filewatch::PathType::FILE);

        REQUIRE(filter.passes(evCpp) == true);
        REQUIRE(filter.passes(evH) == false);

        // 测试非法正则表达式不会导致崩溃，并自动禁用模式匹配
        filter.setPathPattern("[invalid(regex");
        REQUIRE(filter.passes(evH) == true);
    }

    SECTION("Combined filters and chaining") {
        filewatch::FileWatcher::Filter filter;
        filter.setEventTypes({filewatch::EventType::kModify})
              .setExtensions({".txt"})
              .setPathPattern(".*/data/.*");

        filewatch::FileEvent matchEv(filewatch::EventType::kModify, "/root/data/file.txt", filewatch::PathType::FILE);
        filewatch::FileEvent diffType(filewatch::EventType::kCreate, "/root/data/file.txt", filewatch::PathType::FILE);
        filewatch::FileEvent diffExt(filewatch::EventType::kModify, "/root/data/file.doc", filewatch::PathType::FILE);
        filewatch::FileEvent diffPath(filewatch::EventType::kModify, "/root/other/file.txt", filewatch::PathType::FILE);

        REQUIRE(filter.passes(matchEv) == true);
        REQUIRE(filter.passes(diffType) == false);
        REQUIRE(filter.passes(diffExt) == false);
        REQUIRE(filter.passes(diffPath) == false);
    }

    SECTION("Exclude paths filtering") {
        filewatch::FileWatcher::Filter filter;
        filter.addExcludePath("/home/tony/work/temp")
              .addExcludePath("/home/tony/work/ignored_dir");

        filewatch::FileEvent evNormal(filewatch::EventType::kCreate, "/home/tony/work/main.cpp", filewatch::PathType::FILE);
        filewatch::FileEvent evIgnoredSelf(filewatch::EventType::kModify, "/home/tony/work/temp", filewatch::PathType::DIRECTORY);
        filewatch::FileEvent evIgnoredChild(filewatch::EventType::kCreate, "/home/tony/work/temp/sub/cache.tmp", filewatch::PathType::FILE);
        filewatch::FileEvent evIgnoredDirChild(filewatch::EventType::kDelete, "/home/tony/work/ignored_dir/log.txt", filewatch::PathType::FILE);
        filewatch::FileEvent evSimilarName(filewatch::EventType::kCreate, "/home/tony/work/temporary.cpp", filewatch::PathType::FILE);

        REQUIRE(filter.passes(evNormal) == true);
        REQUIRE(filter.passes(evIgnoredSelf) == false);
        REQUIRE(filter.passes(evIgnoredChild) == false);
        REQUIRE(filter.passes(evIgnoredDirChild) == false);
        // /home/tony/work/temporary.cpp 虽然前缀包含 temp，但不是其子目录或完全相等，不应该被误伤
        REQUIRE(filter.passes(evSimilarName) == true);
    }

    SECTION("Exclude regex patterns filtering (e.g. build-* and .git)") {
        filewatch::FileWatcher::Filter filter;
        // 排除任意以 build- 开头的目录（不带文件扩展名点号）及其子文件，以及 .git 目录
        filter.addExcludePattern(R"((^|[/\\])build-[^./\\]+([/\\]|$))")
              .addExcludePattern(R"((^|[/\\])\.git([/\\]|$))");

        filewatch::FileEvent evSrc(filewatch::EventType::kCreate, "/home/tony/work/src/main.cpp", filewatch::PathType::FILE);
        filewatch::FileEvent evBuildDebugDir(filewatch::EventType::kCreate, "/home/tony/work/build-debug", filewatch::PathType::DIRECTORY);
        filewatch::FileEvent evBuildDebugObj(filewatch::EventType::kModify, "/home/tony/work/build-debug/main.o", filewatch::PathType::FILE);
        filewatch::FileEvent evNestedBuild(filewatch::EventType::kCreate, "/home/tony/work/subproject/build-release/lib.a", filewatch::PathType::FILE);
        filewatch::FileEvent evGit(filewatch::EventType::kModify, "/home/tony/work/.git/index", filewatch::PathType::FILE);
        filewatch::FileEvent evBuildScript(filewatch::EventType::kCreate, "/home/tony/work/src/build-script.py", filewatch::PathType::FILE);

        REQUIRE(filter.passes(evSrc) == true);
        REQUIRE(filter.passes(evBuildDebugDir) == false);
        REQUIRE(filter.passes(evBuildDebugObj) == false);
        REQUIRE(filter.passes(evNestedBuild) == false);
        REQUIRE(filter.passes(evGit) == false);
        // build-script.py 带有 .py 后缀并非纯目录名，不应被排除
        REQUIRE(filter.passes(evBuildScript) == true);
    }
}

// 测试错误码与错误分类 (errc & watcher_error_category)
TEST_CASE("Error category and error code tests", "[Error]") {
    SECTION("watcher_category singleton and properties") {
        const auto& cat1 = filewatch::errc::watcher_category();
        const auto& cat2 = filewatch::errc::watcher_category();
        REQUIRE(&cat1 == &cat2);
        REQUIRE(std::string(cat1.name()) == "filewatch");
    }

    SECTION("Error messages mapping") {
        const auto& cat = filewatch::errc::watcher_category();
        REQUIRE(cat.message(static_cast<int>(filewatch::errc::watcher_errc::success)) == "success");
        REQUIRE(cat.message(static_cast<int>(filewatch::errc::watcher_errc::invalid_path)) == "invalid path");
        REQUIRE(cat.message(static_cast<int>(filewatch::errc::watcher_errc::path_not_exist)) == "path does not exist");
        REQUIRE(cat.message(static_cast<int>(filewatch::errc::watcher_errc::permission_denied)) == "permission denied");
        REQUIRE(cat.message(static_cast<int>(filewatch::errc::watcher_errc::platform_error)) == "platform error");
        REQUIRE(cat.message(static_cast<int>(filewatch::errc::watcher_errc::regex_error)) == "regex error");
        REQUIRE(cat.message(static_cast<int>(filewatch::errc::watcher_errc::internal_error)) == "internal error");
        REQUIRE(cat.message(static_cast<int>(filewatch::errc::watcher_errc::already_watching)) == "already watching");
        REQUIRE(cat.message(static_cast<int>(filewatch::errc::watcher_errc::watcher_not_started)) == "watcher not started");
        REQUIRE(cat.message(static_cast<int>(filewatch::errc::watcher_errc::watcher_already_running)) == "watcher already running");
        REQUIRE(cat.message(static_cast<int>(filewatch::errc::watcher_errc::unknown_error)) == "unknown error");
        REQUIRE(cat.message(99999) == "unknown error");
    }

    SECTION("make_error_code and std::error_code integration") {
        std::error_code ec = filewatch::make_error_code(filewatch::errc::watcher_errc::path_not_exist);
        REQUIRE(ec.value() == static_cast<int>(filewatch::errc::watcher_errc::path_not_exist));
        REQUIRE(ec.category() == filewatch::errc::watcher_category());
        REQUIRE(ec == filewatch::errc::watcher_errc::path_not_exist);
        REQUIRE(ec.message() == "path does not exist");
    }
}

// 测试定时器管理（Timer，基于 libevent 模式，外部持有对象，主线程无锁回调）
TEST_CASE("Timer class and management tests", "[Timer]") {
    SECTION("Zero timers case: watcher starts, does not block indefinitely, stops cleanly") {
        filewatch::FileWatcher watcher;
        std::atomic<bool> started(false);
        auto t0 = std::chrono::steady_clock::now();

        std::thread startThread([&watcher, &started]() {
            started = watcher.start();
        });

        // 验证 0 定时器下不会崩溃，等待 200ms 后正常 stop
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        watcher.stop();
        startThread.join();

        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        REQUIRE(started == true);
        REQUIRE(elapsedMs < 2000); // 必须在合理时间内退出，不能永久阻塞
    }

    SECTION("One-shot timer triggers once accurately and on main thread") {
        filewatch::FileWatcher watcher;
        std::thread::id mainThreadId;
        std::atomic<int> triggerCount(0);
        std::thread::id callbackThreadId;

        // 外部栈持有 Timer 对象，零动态分配
        filewatch::Timer timer([&triggerCount, &callbackThreadId](filewatch::Timer& t) {
            triggerCount++;
            callbackThreadId = std::this_thread::get_id();
            REQUIRE(t.isActive() == false); // 单次定时器触发时已被置为 inactive
        });

        REQUIRE(timer.isActive() == false);
        bool added = watcher.addTimer(timer, 100);
        REQUIRE(added == true);
        REQUIRE(timer.isActive() == true);
        REQUIRE(timer.getIntervalMs() == 100);
        REQUIRE(timer.isRepeating() == false);

        std::thread startThread([&watcher, &mainThreadId]() {
            mainThreadId = std::this_thread::get_id();
            watcher.start();
        });

        // 等待足够时间触发单次定时器
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        watcher.stop();
        startThread.join();

        REQUIRE(triggerCount == 1);
        REQUIRE(callbackThreadId == mainThreadId); // 回调 100% 在主线程执行
        REQUIRE(timer.isActive() == false);
    }

    SECTION("Repeating timer triggers multiple times periodically") {
        filewatch::FileWatcher watcher;
        std::atomic<int> triggerCount(0);

        // 外部持有周期性定时器（repeating = true）
        filewatch::Timer timer([&triggerCount]() {
            triggerCount++;
        }, true);

        REQUIRE(timer.isRepeating() == true);
        watcher.addTimer(timer, 80);

        std::thread startThread([&watcher]() {
            watcher.start();
        });

        // 运行 280ms，应该触发 3 次左右 (80ms, 160ms, 240ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(280));

        watcher.stop();
        startThread.join();

        REQUIRE(triggerCount >= 2);
        REQUIRE(triggerCount <= 5);
    }

    SECTION("Removing timer before expiry prevents callback execution") {
        filewatch::FileWatcher watcher;
        std::atomic<int> triggerCount(0);

        filewatch::Timer timer([&triggerCount]() {
            triggerCount++;
        });

        watcher.addTimer(timer, 250);
        REQUIRE(timer.isActive() == true);

        std::thread startThread([&watcher]() {
            watcher.start();
        });

        // 50ms 时提前移除定时器
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        bool removed = watcher.removeTimer(timer);
        REQUIRE(removed == true);
        REQUIRE(timer.isActive() == false);

        // 再等待 300ms，确保超出原本的 250ms
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        watcher.stop();
        startThread.join();

        REQUIRE(triggerCount == 0); // 取消成功，绝对未被触发
    }

    SECTION("Callback adding another timer does not deadlock and executes properly") {
        filewatch::FileWatcher watcher;
        std::atomic<int> step(0);

        filewatch::Timer timerB([&step]() {
            step = 2;
        });

        filewatch::Timer timerA([&watcher, &timerB, &step]() {
            step = 1;
            // 在主线程回调内部添加新的定时器 B
            watcher.addTimer(timerB, 60);
        });

        watcher.addTimer(timerA, 60);

        std::thread startThread([&watcher]() {
            watcher.start();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        watcher.stop();
        startThread.join();

        REQUIRE(step == 2);
    }

    SECTION("Timer object reuse: can be re-added after expiration without allocation") {
        filewatch::FileWatcher watcher;
        std::atomic<int> triggerCount(0);

        filewatch::Timer timer([&triggerCount]() {
            triggerCount++;
        });

        watcher.addTimer(timer, 60);

        std::thread startThread([&watcher]() {
            watcher.start();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        REQUIRE(triggerCount == 1);
        REQUIRE(timer.isActive() == false);

        // 复用同一个 Timer 实体重新注册
        watcher.addTimer(timer, 60);
        REQUIRE(timer.isActive() == true);

        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        REQUIRE(triggerCount == 2);

        watcher.stop();
        startThread.join();
    }

    SECTION("Multiple timers execute in correct order based on expiration") {
        filewatch::FileWatcher watcher;
        std::vector<int> order;

        filewatch::Timer timer1([&order]() {
            order.push_back(1);
        });
        filewatch::Timer timer2([&order]() {
            order.push_back(2);
        });
        filewatch::Timer timer3([&order]() {
            order.push_back(3);
        });

        // 乱序添加，但按时间先后应该为 1 (50ms) -> 2 (120ms) -> 3 (200ms)
        watcher.addTimer(timer3, 200);
        watcher.addTimer(timer1, 50);
        watcher.addTimer(timer2, 120);

        std::thread startThread([&watcher]() {
            watcher.start();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        watcher.stop();
        startThread.join();

        REQUIRE(order.size() == 3);
        if (order.size() == 3) {
            REQUIRE(order[0] == 1);
            REQUIRE(order[1] == 2);
            REQUIRE(order[2] == 3);
        }
    }

    SECTION("Callback cancelling repeating timer itself stops further triggers") {
        filewatch::FileWatcher watcher;
        std::atomic<int> triggerCount(0);

        filewatch::Timer timer;
        timer.setRepeating(true);
        timer.setCallback([&watcher, &timer, &triggerCount](filewatch::Timer& t) {
            triggerCount++;
            if (triggerCount == 2) {
                // 在触发第 2 次时在回调内取消自身
                watcher.removeTimer(t);
            }
        });

        watcher.addTimer(timer, 60);

        std::thread startThread([&watcher]() {
            watcher.start();
        });

        // 运行 250ms，原本应该触发约 4 次，但因在第 2 次取消，triggerCount 必须恒为 2
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        watcher.stop();
        startThread.join();

        REQUIRE(triggerCount == 2);
        REQUIRE(timer.isActive() == false);
    }

    SECTION("Callback cancelling another pending timer prevents it from firing") {
        filewatch::FileWatcher watcher;
        std::atomic<bool> timerBExecuted(false);

        filewatch::Timer timerB([&timerBExecuted]() {
            timerBExecuted = true;
        });

        filewatch::Timer timerA([&watcher, &timerB]() {
            // A 到期时立即取消 B
            watcher.removeTimer(timerB);
        });

        // A 设为 60ms，B 设为 120ms
        watcher.addTimer(timerA, 60);
        watcher.addTimer(timerB, 120);

        std::thread startThread([&watcher]() {
            watcher.start();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        watcher.stop();
        startThread.join();

        REQUIRE(timerBExecuted == false);
        REQUIRE(timerB.isActive() == false);
    }

    SECTION("Timer move semantics and state transfer") {
        bool called = false;
        filewatch::Timer t1([&called]() {
            called = true;
        }, true);
        t1.setRepeating(true);

        REQUIRE(t1.isRepeating() == true);
        REQUIRE(t1.isActive() == false);

        // 移动构造
        filewatch::Timer t2(std::move(t1));
        REQUIRE(t2.isRepeating() == true);
        REQUIRE(t1.isActive() == false);

        // 移动赋值
        filewatch::Timer t3;
        t3 = std::move(t2);
        REQUIRE(t3.isRepeating() == true);
        REQUIRE(t2.isActive() == false);
    }
}


