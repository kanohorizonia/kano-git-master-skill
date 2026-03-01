cmake_minimum_required(VERSION 3.21)
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
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>"
        "$<$<COMPILE_LANGUAGE:CXX>:/GR>"
    )
endif()

# Get version from parent VERSION file
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" KOG_VERSION_FILE_CONTENT)
    string(STRIP "${KOG_VERSION_FILE_CONTENT}" KOG_VERSION_STR)
else()
    set(KOG_VERSION_STR "0.0.0-dev")
endif()
add_compile_definitions(KOG_VERSION="${KOG_VERSION_STR}")

set(KOG_BUILD_VERSION_STR "${KOG_VERSION_STR}")
set(KOG_BUILD_VCS_STR "$ENV{KOG_BUILD_VCS}")
set(KOG_BUILD_BRANCH_STR "$ENV{KOG_BUILD_BRANCH}")
set(KOG_BUILD_REVISION_STR "$ENV{KOG_BUILD_REVISION}")
set(KOG_BUILD_REVISION_HASH_SHORT_STR "$ENV{KOG_BUILD_REVISION_HASH_SHORT}")
set(KOG_BUILD_REVISION_HASH_STR "$ENV{KOG_BUILD_REVISION_HASH}")
set(KOG_BUILD_DIRTY_STR "$ENV{KOG_BUILD_DIRTY}")
set(KOG_BUILD_TIMESTAMP_UTC_STR "$ENV{KOG_BUILD_TIMESTAMP_UTC}")
set(KOG_BUILD_HOST_NAME_STR "$ENV{KOG_BUILD_HOST_NAME}")
set(KOG_BUILD_CI_STR "$ENV{KOG_BUILD_CI}")
set(KOG_BUILD_PIPELINE_ID_STR "$ENV{KOG_BUILD_PIPELINE_ID}")
set(KOG_BUILD_PLATFORM_STR "$ENV{KOG_BUILD_PLATFORM}")
set(KOG_BUILD_TOOLCHAIN_STR "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

if(KOG_BUILD_VCS_STR STREQUAL "")
    set(KOG_BUILD_VCS_STR "unknown")
endif()
if(KOG_BUILD_BRANCH_STR STREQUAL "")
    set(KOG_BUILD_BRANCH_STR "unknown")
endif()
if(KOG_BUILD_REVISION_STR STREQUAL "")
    set(KOG_BUILD_REVISION_STR "unknown")
endif()
if(KOG_BUILD_REVISION_HASH_SHORT_STR STREQUAL "")
    set(KOG_BUILD_REVISION_HASH_SHORT_STR "unknown")
endif()
if(KOG_BUILD_REVISION_HASH_STR STREQUAL "")
    set(KOG_BUILD_REVISION_HASH_STR "unknown")
endif()
if(KOG_BUILD_DIRTY_STR STREQUAL "")
    set(KOG_BUILD_DIRTY_STR "unknown")
endif()
if(KOG_BUILD_TIMESTAMP_UTC_STR STREQUAL "")
    set(KOG_BUILD_TIMESTAMP_UTC_STR "unknown")
endif()
if(KOG_BUILD_HOST_NAME_STR STREQUAL "")
    set(KOG_BUILD_HOST_NAME_STR "unknown")
endif()
if(KOG_BUILD_CI_STR STREQUAL "")
    set(KOG_BUILD_CI_STR "false")
endif()
if(KOG_BUILD_PIPELINE_ID_STR STREQUAL "")
    set(KOG_BUILD_PIPELINE_ID_STR "unknown")
endif()
if(KOG_BUILD_PLATFORM_STR STREQUAL "")
    set(KOG_BUILD_PLATFORM_STR "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
endif()

add_compile_definitions(KOG_BUILD_VERSION="${KOG_BUILD_VERSION_STR}")
add_compile_definitions(KOG_BUILD_VCS="${KOG_BUILD_VCS_STR}")
add_compile_definitions(KOG_BUILD_BRANCH="${KOG_BUILD_BRANCH_STR}")
add_compile_definitions(KOG_BUILD_REVISION="${KOG_BUILD_REVISION_STR}")
add_compile_definitions(KOG_BUILD_REVISION_HASH_SHORT="${KOG_BUILD_REVISION_HASH_SHORT_STR}")
add_compile_definitions(KOG_BUILD_REVISION_HASH="${KOG_BUILD_REVISION_HASH_STR}")
add_compile_definitions(KOG_BUILD_DIRTY="${KOG_BUILD_DIRTY_STR}")
add_compile_definitions(KOG_BUILD_TIMESTAMP_UTC="${KOG_BUILD_TIMESTAMP_UTC_STR}")
add_compile_definitions(KOG_BUILD_HOST_NAME="${KOG_BUILD_HOST_NAME_STR}")
add_compile_definitions(KOG_BUILD_CI="${KOG_BUILD_CI_STR}")
add_compile_definitions(KOG_BUILD_PIPELINE_ID="${KOG_BUILD_PIPELINE_ID_STR}")
add_compile_definitions(KOG_BUILD_PLATFORM="${KOG_BUILD_PLATFORM_STR}")
add_compile_definitions(KOG_BUILD_TOOLCHAIN="${KOG_BUILD_TOOLCHAIN_STR}")
# CLI11 v2.4 uses <codecvt> which is removed in MSVC 19.50 (VS 2026)
add_compile_definitions(CLI11_HAS_CODECVT=0)

# Resolve CLI11: prefer vendored source in code/thirdparty/cli11
set(KOG_CLI11_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/code/thirdparty/cli11")
set(KOG_CLI11_BINARY_DIR "${CMAKE_BINARY_DIR}/thirdparty/cli11-build")

if(EXISTS "${KOG_CLI11_SOURCE_DIR}/CMakeLists.txt")
    add_subdirectory("${KOG_CLI11_SOURCE_DIR}" "${KOG_CLI11_BINARY_DIR}" EXCLUDE_FROM_ALL)
    set(cli11_SOURCE_DIR "${KOG_CLI11_SOURCE_DIR}")
else()
    find_package(CLI11 CONFIG QUIET)
endif()

if(NOT EXISTS "${KOG_CLI11_SOURCE_DIR}/CMakeLists.txt" AND NOT CLI11_FOUND)
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

set(KOG_FTXUI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/code/thirdparty/ftxui")
set(KOG_FTXUI_BINARY_DIR "${CMAKE_BINARY_DIR}/thirdparty/ftxui-build")

if(EXISTS "${KOG_FTXUI_SOURCE_DIR}/CMakeLists.txt")
    add_subdirectory("${KOG_FTXUI_SOURCE_DIR}" "${KOG_FTXUI_BINARY_DIR}" EXCLUDE_FROM_ALL)
    message(STATUS "FTXUI vendored source found: enabling full tui")
else()
    message(FATAL_ERROR "FTXUI vendored source not found at code/thirdparty/ftxui. Please clone fork+upstream into thirdparty first.")
endif()

# Avoid mutating vendored sources; use compile definition only.


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
