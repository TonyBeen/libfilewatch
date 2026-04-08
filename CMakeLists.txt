cmake_minimum_required(VERSION 3.10)
project(libfilewatch VERSION 0.1.0 LANGUAGES CXX)

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
    include/filewatch/LinuxBackend.h
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