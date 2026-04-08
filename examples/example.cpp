#include "filewatch/FileWatcher.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* eventTypeToString(filewatch::EventType type) {
    switch (type) {
        case filewatch::EventType::kCreate: return "CREATE";
        case filewatch::EventType::kModify: return "MODIFY";
        case filewatch::EventType::kDelete: return "DELETE";
        case filewatch::EventType::kRename: return "RENAME";
        default: return "UNKNOWN";
    }
}

const char* pathTypeToString(filewatch::PathType type) {
    return type == filewatch::PathType::DIRECTORY ? "DIR" : "FILE";
}

struct Options {
    std::vector<std::string> directories;
    std::vector<std::string> files;
    std::vector<filewatch::EventType> eventTypes;
    std::string regexPattern;
    bool hasRegex;
    bool recursive;
};

void printUsage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n"
        << "\nOptions:\n"
        << "  --dir <path>          Add a directory to watch (repeatable)\n"
        << "  --file <path>         Add a file to watch (repeatable)\n"
        << "  --recursive           Watch directories recursively\n"
        << "  --events <list>       Comma-separated events: create,modify,delete,rename\n"
        << "  --regex <pattern>     Regex filter for directory watch paths\n"
        << "  --help                Show this help\n"
        << "\nExamples:\n"
        << "  filewatch_example --dir ./logs --recursive --events create,modify\n"
        << "  filewatch_example --dir ./logs --regex '.*\\.(log|txt)$'\n"
        << "  filewatch_example --file /tmp/a.txt --file /tmp/b.txt\n";
}

std::string toLower(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

bool parseEventType(const std::string& token, filewatch::EventType& type) {
    const std::string name = toLower(token);
    if (name == "create") {
        type = filewatch::EventType::kCreate;
        return true;
    }
    if (name == "modify") {
        type = filewatch::EventType::kModify;
        return true;
    }
    if (name == "delete") {
        type = filewatch::EventType::kDelete;
        return true;
    }
    if (name == "rename") {
        type = filewatch::EventType::kRename;
        return true;
    }
    return false;
}

bool parseEvents(const std::string& text, std::vector<filewatch::EventType>& events) {
    std::set<int> seen;
    std::stringstream ss(text);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }
        filewatch::EventType type = filewatch::EventType::kModify;
        if (!parseEventType(token, type)) {
            return false;
        }
        const int key = static_cast<int>(type);
        if (seen.insert(key).second) {
            events.push_back(type);
        }
    }
    return true;
}

bool parseArgs(int argc, char** argv, Options& options, bool& showHelp) {
    options.recursive = false;
    options.hasRegex = false;
    showHelp = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            showHelp = true;
            return false;
        }
        if (arg == "--recursive") {
            options.recursive = true;
            continue;
        }
        if (arg == "--dir" || arg == "--file" || arg == "--events" || arg == "--regex") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for option: " << arg << std::endl;
                return false;
            }
            const std::string value = argv[++i];
            if (arg == "--dir") {
                options.directories.push_back(value);
            } else if (arg == "--file") {
                options.files.push_back(value);
            } else if (arg == "--events") {
                if (!parseEvents(value, options.eventTypes)) {
                    std::cerr << "Invalid events list: " << value << std::endl;
                    return false;
                }
            } else {
                options.regexPattern = value;
                options.hasRegex = true;
            }
            continue;
        }

        std::cerr << "Unknown option: " << arg << std::endl;
        return false;
    }

    if (options.directories.empty() && options.files.empty()) {
        options.directories.push_back(".");
    }
    return true;
}

void printConfig(const Options& options) {
    std::cout << "Watch configuration:" << std::endl;
    std::cout << "  Recursive: " << (options.recursive ? "true" : "false") << std::endl;

    std::cout << "  Directories:";
    if (options.directories.empty()) {
        std::cout << " (none)";
    }
    std::cout << std::endl;
    for (size_t i = 0; i < options.directories.size(); ++i) {
        std::cout << "    - [" << i << "] " << options.directories[i] << std::endl;
    }

    std::cout << "  Files:";
    if (options.files.empty()) {
        std::cout << " (none)";
    }
    std::cout << std::endl;
    for (size_t i = 0; i < options.files.size(); ++i) {
        std::cout << "    - [" << i << "] " << options.files[i] << std::endl;
    }

    std::cout << "  Events:";
    if (options.eventTypes.empty()) {
        std::cout << " all";
    } else {
        for (size_t i = 0; i < options.eventTypes.size(); ++i) {
            std::cout << (i == 0 ? " " : ", ") << eventTypeToString(options.eventTypes[i]);
        }
    }
    std::cout << std::endl;

    std::cout << "  Regex: " << (options.hasRegex ? options.regexPattern : "(none)") << std::endl;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    bool showHelp = false;
    if (!parseArgs(argc, argv, options, showHelp)) {
        printUsage(argv[0]);
        return showHelp ? 0 : 1;
    }

    filewatch::FileWatcher watcher(filewatch::LinuxBackend::INOTIFY);

    if (!options.eventTypes.empty()) {
        filewatch::FileWatcher::Filter filter;
        filter.setEventTypes(options.eventTypes);
        watcher.setFilter(filter);
    }

    filewatch::FileWatcher::EventCallback callback = [](const filewatch::FileEvent& event) {
        std::cout << "[" << eventTypeToString(event.getType()) << "] "
                  << "(" << pathTypeToString(event.getPathType()) << ") "
                  << event.getPath();

        if (event.getType() == filewatch::EventType::kRename) {
            std::cout << " <= " << event.getOldPath();
        }
        std::cout << std::endl;
    };

    for (size_t i = 0; i < options.directories.size(); ++i) {
        bool ok = false;
        if (options.hasRegex) {
            ok = watcher.addWatchWithRegex(options.directories[i], options.regexPattern, options.recursive, callback);
        } else {
            ok = watcher.addWatch(options.directories[i], options.recursive, callback);
        }
        if (!ok) {
            std::cerr << "addWatch directory failed: " << options.directories[i]
                      << " error=" << watcher.getLastError().message() << std::endl;
            return 1;
        }
    }

    for (size_t i = 0; i < options.files.size(); ++i) {
        if (!watcher.addWatch(options.files[i], false, callback)) {
            std::cerr << "addWatch file failed: " << options.files[i]
                      << " error=" << watcher.getLastError().message() << std::endl;
            return 1;
        }
    }

    printConfig(options);

    bool started = false;
    std::thread watchThread([&watcher, &started]() {
        started = watcher.start();
    });

    std::cout << "Press Enter to stop..." << std::endl;
    std::cin.get();

    watcher.stop();
    watchThread.join();

    if (!started) {
        std::cerr << "watcher start failed: " << watcher.getLastError().message() << std::endl;
        return 1;
    }

    return 0;
}