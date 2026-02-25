cmake_minimum_required(VERSION 3.28)
project(KanoGit DESCRIPTION "CLI" VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_INCLUDE_SYSTEM_FLAG_CXX "/I ")

# Get version from parent VERSION file
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" KOG_VERSION_FILE_CONTENT)
    string(STRIP "${KOG_VERSION_FILE_CONTENT}" KOG_VERSION_STR)
else()
    set(KOG_VERSION_STR "0.0.0-dev")
endif()
add_compile_definitions(KOG_VERSION="${KOG_VERSION_STR}")
# CLI11 v2.4 uses <codecvt> which is removed in MSVC 19.50 (VS 2026)
add_compile_definitions(CLI11_HAS_CODECVT=0)

# Find dependencies or fallback to FetchContent
find_package(CLI11 CONFIG QUIET)
if(NOT CLI11_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        CLI11
        GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
        GIT_TAG v2.4.2
    )
    FetchContent_MakeAvailable(CLI11)
    set_target_properties(CLI11 PROPERTIES SYSTEM OFF)
    # Patch CLI11 for MSVC 19.50+ (VS 2026) which removed <codecvt>
    set(_CLI11_MACROS "${cli11_SOURCE_DIR}/include/CLI/Macros.hpp")
    if(EXISTS "${_CLI11_MACROS}")
        file(READ "${_CLI11_MACROS}" _macros_content)
        string(FIND "${_macros_content}" "#ifndef CLI11_HAS_CODECVT" _already_patched)
        if(_already_patched EQUAL -1)
            string(REPLACE
                "/** <codecvt> availability */\n#if defined(__GNUC__) && !defined(__llvm__) && !defined(__INTEL_COMPILER) && __GNUC__ < 5\n#define CLI11_HAS_CODECVT 0\n#else\n#define CLI11_HAS_CODECVT 1\n#include <codecvt>\n#endif"
                "/** <codecvt> availability */\n#ifndef CLI11_HAS_CODECVT\n#if defined(__GNUC__) && !defined(__llvm__) && !defined(__INTEL_COMPILER) && __GNUC__ < 5\n#define CLI11_HAS_CODECVT 0\n#elif defined(_MSC_VER) && _MSC_VER >= 1950\n#define CLI11_HAS_CODECVT 0\n#else\n#define CLI11_HAS_CODECVT 1\n#include <codecvt>\n#endif\n#endif"
                _macros_content "${_macros_content}")
            file(WRITE "${_CLI11_MACROS}" "${_macros_content}")
            message(STATUS "Patched CLI11 Macros.hpp: disabled <codecvt> for MSVC 19.50+")
        endif()
    endif()
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
