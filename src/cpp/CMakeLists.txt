cmake_minimum_required(VERSION 3.28)
project(KanoGit DESCRIPTION "CLI" VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(KOG_ENABLE_MODULES "Enable C++20 modules" ON)
if(KOG_ENABLE_MODULES)
    set(CMAKE_CXX_SCAN_FOR_MODULES ON)
else()
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
endif()
if(MSVC)
    set(CMAKE_INCLUDE_SYSTEM_FLAG_CXX "/I ")
    set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)
endif()

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

# Resolve CLI11: prefer vendored source in code/thirdparty/cli11
set(KOG_CLI11_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/code/thirdparty/cli11")
set(KOG_CLI11_BINARY_DIR "${CMAKE_BINARY_DIR}/thirdparty/cli11-build")

find_package(CLI11 CONFIG QUIET)
if(EXISTS "${KOG_CLI11_SOURCE_DIR}/CMakeLists.txt")
    add_subdirectory("${KOG_CLI11_SOURCE_DIR}" "${KOG_CLI11_BINARY_DIR}" EXCLUDE_FROM_ALL)
    set(cli11_SOURCE_DIR "${KOG_CLI11_SOURCE_DIR}")
elseif(NOT CLI11_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        CLI11
        GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
        GIT_TAG v2.4.2
        SOURCE_DIR "${KOG_CLI11_SOURCE_DIR}"
        BINARY_DIR "${KOG_CLI11_BINARY_DIR}"
        SUBBUILD_DIR "${CMAKE_BINARY_DIR}/thirdparty/cli11-subbuild"
    )
    FetchContent_GetProperties(CLI11)
    if(NOT CLI11_POPULATED)
        FetchContent_Populate(CLI11)
        add_subdirectory("${KOG_CLI11_SOURCE_DIR}" "${KOG_CLI11_BINARY_DIR}" EXCLUDE_FROM_ALL)
    endif()
    set(cli11_SOURCE_DIR "${KOG_CLI11_SOURCE_DIR}")
endif()

# Patch CLI11 for MSVC 19.50+ (VS 2026) which removed <codecvt>
if(DEFINED cli11_SOURCE_DIR)
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
set(KOG_BUILD_ROOT "${CMAKE_SOURCE_DIR}/build")
set(KOG_PRESET_NAME "unknown-preset" CACHE STRING "CMake preset name for artifact layout")

set(KOG_BIN_ROOT "${KOG_BUILD_ROOT}/bin/${KOG_PRESET_NAME}")
set(KOG_LIB_ROOT "${KOG_BUILD_ROOT}/lib/${KOG_PRESET_NAME}")
set(KOG_SYMBOL_ROOT "${KOG_BUILD_ROOT}/_intermediate/${KOG_PRESET_NAME}/symbols")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${KOG_BIN_ROOT}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${KOG_LIB_ROOT}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${KOG_LIB_ROOT}")

set(CMAKE_PDB_OUTPUT_DIRECTORY "${KOG_SYMBOL_ROOT}")
set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY "${KOG_SYMBOL_ROOT}")

foreach(cfg IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${cfg}" cfg_upper)
    string(TOLOWER "${cfg}" cfg_lower)

    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} "${KOG_BIN_ROOT}/${cfg_lower}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${cfg_upper} "${KOG_LIB_ROOT}/${cfg_lower}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${cfg_upper} "${KOG_LIB_ROOT}/${cfg_lower}")
    set(CMAKE_PDB_OUTPUT_DIRECTORY_${cfg_upper} "${KOG_SYMBOL_ROOT}/${cfg_lower}")
    set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY_${cfg_upper} "${KOG_SYMBOL_ROOT}/${cfg_lower}")
endforeach()

# Add subdirectories
add_subdirectory(code/systems/kano_git_core)
add_subdirectory(code/apps/kano_git_cli)
