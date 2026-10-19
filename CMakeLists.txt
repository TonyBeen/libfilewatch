cmake_minimum_required(VERSION 3.10)

# 获取 git 提交次数并计算版本号（以 0.1.0 为基点，每提交 1 次计数加 1，10 进制进位：如 11 次对应 0.2.1）
find_package(Git QUIET)
set(LIBFILEWATCH_COMMIT_COUNT 0)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE _git_commit_count
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _git_res
    )
    if(_git_res EQUAL 0 AND _git_commit_count MATCHES "^[0-9]+$")
        set(LIBFILEWATCH_COMMIT_COUNT ${_git_commit_count})
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git/index")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/.git/index")
    endif()
endif()

math(EXPR _total_version_number "10 + ${LIBFILEWATCH_COMMIT_COUNT}")
math(EXPR LIBFILEWATCH_VERSION_MAJOR "${_total_version_number} / 100")
math(EXPR LIBFILEWATCH_VERSION_MINOR "(${_total_version_number} % 100) / 10")
math(EXPR LIBFILEWATCH_VERSION_PATCH "${_total_version_number} % 10")
set(LIBFILEWATCH_VERSION_STRING "${LIBFILEWATCH_VERSION_MAJOR}.${LIBFILEWATCH_VERSION_MINOR}.${LIBFILEWATCH_VERSION_PATCH}")

project(libfilewatch VERSION ${LIBFILEWATCH_VERSION_STRING} LANGUAGES CXX)

# 动态生成 Version.h
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/filewatch/Version.h"
    @ONLY
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

option(BUILD_EXAMPLES "Build examples" ON)
option(BUILD_TESTS "Build tests" OFF)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

set(SOURCES
    src/FileWatcher.cpp
    src/Event.cpp
    src/utils/PathUtils.cpp
    src/utils/EncodingUtils.cpp
    src/core/PlatformWatcherFactory.cpp
)

if(WIN32)
    list(APPEND SOURCES src/core/WindowsWatcher.cpp)
elseif(UNIX AND NOT APPLE)
    list(APPEND SOURCES src/core/InotifyWatcher.cpp src/core/FanotifyWatcher.cpp)
elseif(APPLE)
    list(APPEND SOURCES src/core/MacOSWatcher.cpp)
endif()

add_library(filewatch STATIC ${SOURCES})
add_library(libfilewatch::filewatch ALIAS filewatch)
set_target_properties(filewatch PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(filewatch
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(filewatch PRIVATE Threads::Threads)

if(APPLE)
    find_library(CORESERVICES_FRAMEWORK CoreServices REQUIRED)
    target_link_libraries(filewatch PRIVATE ${CORESERVICES_FRAMEWORK})
endif()

install(TARGETS filewatch
    EXPORT libfilewatchTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

set(LIBFILEWATCH_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/libfilewatch")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/libfilewatchConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/libfilewatchConfig.cmake"
    INSTALL_DESTINATION "${LIBFILEWATCH_INSTALL_CMAKEDIR}")

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/libfilewatchConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(EXPORT libfilewatchTargets
    FILE libfilewatchTargets.cmake
    NAMESPACE libfilewatch::
    DESTINATION ${LIBFILEWATCH_INSTALL_CMAKEDIR})

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/libfilewatchConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/libfilewatchConfigVersion.cmake"
    DESTINATION ${LIBFILEWATCH_INSTALL_CMAKEDIR})

install(FILES
    include/filewatch/Event.h
    include/filewatch/FileWatcher.h
    include/filewatch/Timer.h
    include/filewatch/LinuxBackend.h
    "${CMAKE_CURRENT_BINARY_DIR}/include/filewatch/Version.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/filewatch)

if(NOT TARGET uninstall)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
        IMMEDIATE @ONLY)
    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake")
endif()

if(BUILD_EXAMPLES)
    add_executable(filewatch_example examples/example.cpp)
    target_link_libraries(filewatch_example PRIVATE filewatch Threads::Threads)
endif()

if(BUILD_TESTS)
    include(CTest)
    add_executable(unit_test test/unit_test.cpp)
    target_include_directories(unit_test PRIVATE test src)
    target_link_libraries(unit_test PRIVATE filewatch Threads::Threads)
    add_test(NAME unit_test COMMAND unit_test)
endif()