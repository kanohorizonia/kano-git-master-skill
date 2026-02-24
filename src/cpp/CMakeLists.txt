cmake_minimum_required(VERSION 3.28)
project(KanoGit DESCRIPTION "CLI" VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Get version from parent VERSION file
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" KOG_VERSION_FILE_CONTENT)
    string(STRIP "${KOG_VERSION_FILE_CONTENT}" KOG_VERSION_STR)
else()
    set(KOG_VERSION_STR "0.0.0-dev")
endif()
add_compile_definitions(KOG_VERSION="${KOG_VERSION_STR}")

# Find dependencies or fallback to FetchContent
find_package(CLI11 CONFIG QUIET)
if(NOT CLI11_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        CLI11
        GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
        GIT_TAG v2.4.1
    )
    FetchContent_MakeAvailable(CLI11)
endif()

find_package(fmt CONFIG QUIET)
if(NOT fmt_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 10.2.1
    )
    FetchContent_MakeAvailable(fmt)
    set_target_properties(fmt PROPERTIES CXX_STANDARD 20 CXX_EXTENSIONS OFF)
endif()

# Platform detection
if(WIN32)
    add_compile_definitions(KOG_PLATFORM_WINDOWS=1)
else()
    add_compile_definitions(KOG_PLATFORM_UNIX=1)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_BINARY_DIR}/debug)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_BINARY_DIR}/release)

# Add subdirectories
add_subdirectory(kano-git-core)
add_subdirectory(kog-cli)
