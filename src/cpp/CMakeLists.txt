cmake_minimum_required(VERSION 3.21)
project(KanoGit DESCRIPTION "CLI" VERSION 0.1.0 LANGUAGES C CXX)

if(APPLE)
    # The static-library graph intentionally shares transitive dependencies.
    # Suppress duplicate-library linker warnings on Apple ld for cleaner builds.
    add_link_options(-Wl,-no_warn_duplicate_libraries)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(KOG_ENABLE_MODULES "Enable C++20 modules" ON)
option(KOG_ENABLE_UNITY_BUILD "Enable unity build for eligible targets" OFF)
set(KOG_UNITY_BUILD_MODE "full" CACHE STRING "Unity build mode: full|changed")
set_property(CACHE KOG_UNITY_BUILD_MODE PROPERTY STRINGS full changed)
set(KOG_UNITY_CHANGED_FILES "" CACHE STRING "Semicolon-separated repo-relative changed files for selective unity")
set(KOG_UNITY_BATCH_SIZE "0" CACHE STRING "Unity batch size override (0 uses generator default)")
set(KOG_UNITY_EXCLUDED_TARGETS_DEFAULT "kano_git_workspace;kano_git_cmd_repo_sync;kano_git_cmd_commit_plan;kano_git_cmd_product;kano_git_tui_tests")
set(KOG_UNITY_EXCLUDED_TARGETS "" CACHE STRING "Additional semicolon-separated targets excluded from unity build")
set(KOG_UNITY_EXCLUDED_TARGETS_EFFECTIVE ${KOG_UNITY_EXCLUDED_TARGETS_DEFAULT} ${KOG_UNITY_EXCLUDED_TARGETS})
list(REMOVE_DUPLICATES KOG_UNITY_EXCLUDED_TARGETS_EFFECTIVE)
set(KOG_COMPILER_LAUNCHER "auto" CACHE STRING "Compiler launcher: auto|none|ccache|sccache|<absolute-or-program-path>")
set_property(CACHE KOG_COMPILER_LAUNCHER PROPERTY STRINGS auto none ccache sccache)
if(KOG_ENABLE_MODULES)
    set(CMAKE_CXX_SCAN_FOR_MODULES ON)
else()
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
endif()

if(KOG_ENABLE_UNITY_BUILD)
    string(TOLOWER "${KOG_UNITY_BUILD_MODE}" KOG_UNITY_BUILD_MODE_EFFECTIVE)
    if(NOT KOG_UNITY_BUILD_MODE_EFFECTIVE STREQUAL "full" AND NOT KOG_UNITY_BUILD_MODE_EFFECTIVE STREQUAL "changed")
        message(FATAL_ERROR "Unsupported KOG_UNITY_BUILD_MODE: ${KOG_UNITY_BUILD_MODE} (expected full|changed)")
    endif()
else()
    set(KOG_UNITY_BUILD_MODE_EFFECTIVE "off")
endif()

set(KOG_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

function(kog_normalize_repo_relative_path input_path output_var)
    if(input_path STREQUAL "")
        set(${output_var} "" PARENT_SCOPE)
        return()
    endif()

    file(TO_CMAKE_PATH "${input_path}" normalized)
    if(IS_ABSOLUTE "${normalized}")
        file(RELATIVE_PATH normalized "${KOG_REPO_ROOT}" "${normalized}")
    endif()
    string(REGEX REPLACE "^\\./" "" normalized "${normalized}")
    set(${output_var} "${normalized}" PARENT_SCOPE)
endfunction()

function(kog_collect_selective_unity_inputs out_changed_impl out_force_full_reason)
    set(changed_entries)
    set(force_full_reason "")

    if(NOT KOG_UNITY_CHANGED_FILES STREQUAL "")
        set(raw_entries ${KOG_UNITY_CHANGED_FILES})
    else()
        execute_process(
            COMMAND git -C "${KOG_REPO_ROOT}" diff --name-only --diff-filter=ACMRTUXB HEAD --
            OUTPUT_VARIABLE git_diff_output
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        execute_process(
            COMMAND git -C "${KOG_REPO_ROOT}" ls-files --others --exclude-standard --
            OUTPUT_VARIABLE git_untracked_output
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        string(REPLACE "\n" ";" raw_diff_entries "${git_diff_output}")
        string(REPLACE "\n" ";" raw_untracked_entries "${git_untracked_output}")
        set(raw_entries ${raw_diff_entries} ${raw_untracked_entries})
    endif()

    foreach(entry IN LISTS raw_entries)
        if(entry STREQUAL "")
            continue()
        endif()

        kog_normalize_repo_relative_path("${entry}" normalized_entry)
        if(normalized_entry STREQUAL "")
            continue()
        endif()

        get_filename_component(entry_name "${normalized_entry}" NAME)
        get_filename_component(entry_ext "${normalized_entry}" EXT)
        string(TOLOWER "${entry_ext}" entry_ext_lower)

        if(entry_ext_lower STREQUAL ".c" OR entry_ext_lower STREQUAL ".cc" OR entry_ext_lower STREQUAL ".cpp" OR entry_ext_lower STREQUAL ".cxx")
            list(APPEND changed_entries "${normalized_entry}")
        elseif(normalized_entry MATCHES "^src/cpp/" OR entry_name STREQUAL "CMakeLists.txt" OR entry_name STREQUAL "CMakePresets.json" OR entry_name MATCHES "\\.cmake$")
            set(force_full_reason "non-implementation build-affecting change detected: ${normalized_entry}")
        endif()
    endforeach()

    list(REMOVE_DUPLICATES changed_entries)
    set(${out_changed_impl} "${changed_entries}" PARENT_SCOPE)
    set(${out_force_full_reason} "${force_full_reason}" PARENT_SCOPE)
endfunction()

function(kog_collect_buildsystem_targets directory_path output_var)
    get_property(local_targets DIRECTORY "${directory_path}" PROPERTY BUILDSYSTEM_TARGETS)
    set(all_targets ${local_targets})

    get_property(subdirs DIRECTORY "${directory_path}" PROPERTY SUBDIRECTORIES)
    foreach(subdir IN LISTS subdirs)
        kog_collect_buildsystem_targets("${subdir}" child_targets)
        list(APPEND all_targets ${child_targets})
    endforeach()

    list(REMOVE_DUPLICATES all_targets)
    set(${output_var} "${all_targets}" PARENT_SCOPE)
endfunction()

function(kog_configure_unity_target target_name)
    get_target_property(target_type ${target_name} TYPE)
    if(target_type STREQUAL "INTERFACE_LIBRARY" OR target_type STREQUAL "UTILITY")
        return()
    endif()

    get_target_property(target_source_dir ${target_name} SOURCE_DIR)
    get_target_property(target_binary_dir ${target_name} BINARY_DIR)
    if(target_source_dir MATCHES "/code/thirdparty(/|$)" OR target_binary_dir MATCHES "(/|\\\\)_deps(/|\\\\)" OR target_binary_dir MATCHES "(/|\\\\)thirdparty(/|\\\\)")
        set_target_properties(${target_name} PROPERTIES UNITY_BUILD OFF)
        return()
    endif()

    list(FIND KOG_UNITY_EXCLUDED_TARGETS_EFFECTIVE "${target_name}" unity_excluded_target_index)
    if(NOT unity_excluded_target_index EQUAL -1)
        set_target_properties(${target_name} PROPERTIES UNITY_BUILD OFF)
        return()
    endif()

    if(KOG_UNITY_BUILD_MODE_EFFECTIVE STREQUAL "off")
        set_target_properties(${target_name} PROPERTIES UNITY_BUILD OFF)
        return()
    endif()

    set_target_properties(${target_name} PROPERTIES UNITY_BUILD ON)
    string(MAKE_C_IDENTIFIER "KOG_UNITY_${target_name}" unity_unique_id)
    set_target_properties(${target_name} PROPERTIES UNITY_BUILD_UNIQUE_ID "${unity_unique_id}")
    if(NOT KOG_UNITY_BATCH_SIZE STREQUAL "0")
        set_target_properties(${target_name} PROPERTIES UNITY_BUILD_BATCH_SIZE "${KOG_UNITY_BATCH_SIZE}")
    endif()

    if(NOT KOG_UNITY_BUILD_MODE_EFFECTIVE STREQUAL "changed")
        return()
    endif()

    if(NOT KOG_SELECTIVE_UNITY_FORCE_FULL_REASON STREQUAL "")
        return()
    endif()

    get_target_property(target_sources ${target_name} SOURCES)
    if(NOT target_sources)
        return()
    endif()

    if(NOT target_source_dir)
        set(target_source_dir "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    set(target_has_changed_impl OFF)
    foreach(source_path IN LISTS target_sources)
        if(IS_ABSOLUTE "${source_path}")
            set(resolved_source "${source_path}")
        else()
            get_filename_component(resolved_source "${target_source_dir}/${source_path}" ABSOLUTE)
        endif()

        get_filename_component(source_ext "${resolved_source}" EXT)
        string(TOLOWER "${source_ext}" source_ext_lower)
        if(NOT source_ext_lower STREQUAL ".c" AND NOT source_ext_lower STREQUAL ".cc" AND NOT source_ext_lower STREQUAL ".cpp" AND NOT source_ext_lower STREQUAL ".cxx")
            continue()
        endif()

        kog_normalize_repo_relative_path("${resolved_source}" repo_relative_source)
        list(FIND KOG_SELECTIVE_UNITY_CHANGED_IMPL "${repo_relative_source}" changed_index)
        if(NOT changed_index EQUAL -1)
            set(target_has_changed_impl ON)
            break()
        endif()
    endforeach()

    if(NOT target_has_changed_impl)
        set_target_properties(${target_name} PROPERTIES UNITY_BUILD OFF)
        return()
    endif()

    foreach(source_path IN LISTS target_sources)
        if(IS_ABSOLUTE "${source_path}")
            set(resolved_source "${source_path}")
        else()
            get_filename_component(resolved_source "${target_source_dir}/${source_path}" ABSOLUTE)
        endif()

        get_filename_component(source_ext "${resolved_source}" EXT)
        string(TOLOWER "${source_ext}" source_ext_lower)
        if(NOT source_ext_lower STREQUAL ".c" AND NOT source_ext_lower STREQUAL ".cc" AND NOT source_ext_lower STREQUAL ".cpp" AND NOT source_ext_lower STREQUAL ".cxx")
            continue()
        endif()

        kog_normalize_repo_relative_path("${resolved_source}" repo_relative_source)
        list(FIND KOG_SELECTIVE_UNITY_CHANGED_IMPL "${repo_relative_source}" changed_index)
        if(changed_index EQUAL -1)
            set_source_files_properties("${source_path}" TARGET_DIRECTORY ${target_name} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
        else()
            set_source_files_properties("${source_path}" TARGET_DIRECTORY ${target_name} PROPERTIES SKIP_UNITY_BUILD_INCLUSION OFF)
        endif()
    endforeach()
endfunction()

function(kog_apply_unity_configuration)
    kog_collect_buildsystem_targets("${CMAKE_CURRENT_SOURCE_DIR}" all_targets)
    foreach(target_name IN LISTS all_targets)
        kog_configure_unity_target("${target_name}")
    endforeach()
endfunction()

if(KOG_UNITY_BUILD_MODE_EFFECTIVE STREQUAL "changed")
    kog_collect_selective_unity_inputs(KOG_SELECTIVE_UNITY_CHANGED_IMPL KOG_SELECTIVE_UNITY_FORCE_FULL_REASON)
    list(LENGTH KOG_SELECTIVE_UNITY_CHANGED_IMPL KOG_SELECTIVE_UNITY_CHANGED_COUNT)
    if(NOT KOG_SELECTIVE_UNITY_FORCE_FULL_REASON STREQUAL "")
        message(STATUS "Selective unity falling back to full unity: ${KOG_SELECTIVE_UNITY_FORCE_FULL_REASON}")
    else()
        message(STATUS "Selective unity active for ${KOG_SELECTIVE_UNITY_CHANGED_COUNT} changed implementation file(s)")
    endif()
endif()

set(_kog_compiler_launcher_request "${KOG_COMPILER_LAUNCHER}")
if(_kog_compiler_launcher_request STREQUAL "auto" AND DEFINED ENV{KOG_COMPILER_LAUNCHER} AND NOT "$ENV{KOG_COMPILER_LAUNCHER}" STREQUAL "")
    set(_kog_compiler_launcher_request "$ENV{KOG_COMPILER_LAUNCHER}")
endif()

set(_kog_compiler_launcher_path "")
if(_kog_compiler_launcher_request STREQUAL "none")
    message(STATUS "Compiler launcher disabled (KOG_COMPILER_LAUNCHER=none)")
elseif(_kog_compiler_launcher_request STREQUAL "auto")
    if(WIN32)
        find_program(_kog_compiler_launcher_path NAMES ccache sccache)
    else()
        find_program(_kog_compiler_launcher_path NAMES ccache sccache)
    endif()
elseif(_kog_compiler_launcher_request STREQUAL "ccache" OR _kog_compiler_launcher_request STREQUAL "sccache")
    find_program(_kog_compiler_launcher_path NAMES "${_kog_compiler_launcher_request}")
    if(NOT _kog_compiler_launcher_path)
        message(FATAL_ERROR "Requested compiler launcher not found: ${_kog_compiler_launcher_request}")
    endif()
else()
    if(IS_ABSOLUTE "${_kog_compiler_launcher_request}" AND EXISTS "${_kog_compiler_launcher_request}")
        set(_kog_compiler_launcher_path "${_kog_compiler_launcher_request}")
    else()
        find_program(_kog_compiler_launcher_path NAMES "${_kog_compiler_launcher_request}")
        if(NOT _kog_compiler_launcher_path)
            message(FATAL_ERROR "Requested compiler launcher path/program not found: ${_kog_compiler_launcher_request}")
        endif()
    endif()
endif()

if(_kog_compiler_launcher_path)
    set(CMAKE_C_COMPILER_LAUNCHER "${_kog_compiler_launcher_path}" CACHE STRING "C compiler launcher" FORCE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${_kog_compiler_launcher_path}" CACHE STRING "CXX compiler launcher" FORCE)
    message(STATUS "Using compiler launcher: ${_kog_compiler_launcher_path}")
else()
    unset(CMAKE_C_COMPILER_LAUNCHER CACHE)
    unset(CMAKE_CXX_COMPILER_LAUNCHER CACHE)
endif()
if(MSVC)
    set(CMAKE_INCLUDE_SYSTEM_FLAG_CXX "/I ")
    set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>"
        "$<$<COMPILE_LANGUAGE:CXX>:/GR>"
        "$<$<COMPILE_LANGUAGE:CXX>:/utf-8>"
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
set(KOG_BUILD_HOST_NAME_STR "$ENV{KOG_BUILD_HOST_NAME}")
set(KOG_BUILD_CI_STR "$ENV{KOG_BUILD_CI}")
set(KOG_BUILD_CONTEXT_STR "$ENV{KOG_BUILD_CONTEXT}")
set(KOG_BUILD_PIPELINE_ID_STR "$ENV{KOG_BUILD_PIPELINE_ID}")
set(KOG_BUILD_PLATFORM_STR "$ENV{KOG_BUILD_PLATFORM}")
set(KOG_BUILD_TOOLCHAIN_STR "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
set(KOG_BUILD_GENERATOR_STR "${CMAKE_GENERATOR}")
set(KOG_BUILD_PRESET_STR "${KOG_PRESET_NAME}")

if(CMAKE_CONFIGURATION_TYPES)
    set(KOG_BUILD_CONFIGURATION_STR "$<CONFIG>")
else()
    set(KOG_BUILD_CONFIGURATION_STR "${CMAKE_BUILD_TYPE}")
endif()

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
if(KOG_BUILD_HOST_NAME_STR STREQUAL "")
    set(KOG_BUILD_HOST_NAME_STR "unknown")
endif()
if(KOG_BUILD_CI_STR STREQUAL "")
    set(KOG_BUILD_CI_STR "false")
endif()
if(KOG_BUILD_CONTEXT_STR STREQUAL "")
    if(KOG_BUILD_CI_STR STREQUAL "true")
        set(KOG_BUILD_CONTEXT_STR "ci")
    else()
        set(KOG_BUILD_CONTEXT_STR "local-manual")
    endif()
endif()
if(KOG_BUILD_PIPELINE_ID_STR STREQUAL "")
    if(KOG_BUILD_CI_STR STREQUAL "true")
        set(KOG_BUILD_PIPELINE_ID_STR "unknown")
    else()
        set(KOG_BUILD_PIPELINE_ID_STR "local-manual")
    endif()
endif()
if(KOG_BUILD_PLATFORM_STR STREQUAL "")
    set(KOG_BUILD_PLATFORM_STR "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(KOG_BUILD_GENERATOR_STR STREQUAL "")
    set(KOG_BUILD_GENERATOR_STR "unknown")
endif()
if(KOG_BUILD_PRESET_STR STREQUAL "")
    set(KOG_BUILD_PRESET_STR "unknown-preset")
endif()
if(KOG_BUILD_CONFIGURATION_STR STREQUAL "")
    set(KOG_BUILD_CONFIGURATION_STR "unknown")
endif()

# CLI11 v2.4 uses <codecvt> which is removed in MSVC 19.50 (VS 2026)
add_compile_definitions(CLI11_HAS_CODECVT=0)

include(FetchContent)

# Shared FetchContent git defaults:
# - Jenkins Linux/mac lanes have shown intermittent HTTP/2 clone failures when
#   CMake shells out to git for upstream dependencies.
# - Force git to use HTTP/1.1 for FetchContent clones by default.
# - Prefer shallow clones for tag/branch pins, but keep full history for commit
#   hashes because CMake disallows GIT_SHALLOW with commit-pinned GIT_TAG values.
set(KOG_FETCHCONTENT_GIT_HTTP_VERSION "HTTP/1.1" CACHE STRING
    "Git http.version override for FetchContent dependencies; set empty to use git defaults")
option(KOG_FETCHCONTENT_GIT_ENABLE_PROGRESS "Enable progress output for FetchContent git downloads" ON)
option(KOG_FETCHCONTENT_GIT_SHALLOW_TAGS "Use shallow FetchContent git clones for tag/branch-pinned dependencies" ON)

function(kog_fetchcontent_declare_git dependency_name)
    set(options)
    set(one_value_args REPOSITORY TAG)
    set(multi_value_args EXTRA_ARGS)
    cmake_parse_arguments(KOG_FETCH "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT KOG_FETCH_REPOSITORY OR NOT KOG_FETCH_TAG)
        message(FATAL_ERROR
            "kog_fetchcontent_declare_git(${dependency_name}) requires REPOSITORY and TAG")
    endif()

    set(fetch_args
        GIT_REPOSITORY "${KOG_FETCH_REPOSITORY}"
        GIT_TAG "${KOG_FETCH_TAG}"
    )

    if(KOG_FETCHCONTENT_GIT_ENABLE_PROGRESS)
        list(APPEND fetch_args GIT_PROGRESS TRUE)
    endif()

    if(NOT KOG_FETCHCONTENT_GIT_HTTP_VERSION STREQUAL "")
        list(APPEND fetch_args GIT_CONFIG "http.version=${KOG_FETCHCONTENT_GIT_HTTP_VERSION}")
    endif()

    if(KOG_FETCHCONTENT_GIT_SHALLOW_TAGS)
        string(LENGTH "${KOG_FETCH_TAG}" fetch_tag_length)
        set(tag_is_commit_hash FALSE)
        if(KOG_FETCH_TAG MATCHES "^[0-9A-Fa-f]+$" AND fetch_tag_length GREATER_EQUAL 7 AND fetch_tag_length LESS_EQUAL 40)
            set(tag_is_commit_hash TRUE)
        endif()
        if(NOT tag_is_commit_hash)
            list(APPEND fetch_args GIT_SHALLOW TRUE)
        endif()
    endif()

    if(KOG_FETCH_EXTRA_ARGS)
        list(APPEND fetch_args ${KOG_FETCH_EXTRA_ARGS})
    endif()

    FetchContent_Declare(${dependency_name} ${fetch_args})
endfunction()

# Resolve nlohmann/json source strategy
#   fetch (default): FetchContent from upstream release tag into CMake _deps
#   system: use find_package(nlohmann_json)
set(KOG_NLOHMANN_JSON_SOURCE_MODE "fetch" CACHE STRING "nlohmann/json source mode: fetch|system")
set_property(CACHE KOG_NLOHMANN_JSON_SOURCE_MODE PROPERTY STRINGS fetch system)

if(KOG_NLOHMANN_JSON_SOURCE_MODE STREQUAL "fetch")
    kog_fetchcontent_declare_git(
        nlohmann_json
        REPOSITORY https://github.com/nlohmann/json.git
        TAG v3.11.3
    )
    set(NLOHMANN_JSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NLOHMANN_JSON_INSTALL     OFF CACHE BOOL "" FORCE)
    set(JSON_BuildTests           OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(nlohmann_json)
elseif(KOG_NLOHMANN_JSON_SOURCE_MODE STREQUAL "system")
    find_package(nlohmann_json CONFIG REQUIRED)
else()
    message(FATAL_ERROR "Unsupported KOG_NLOHMANN_JSON_SOURCE_MODE: ${KOG_NLOHMANN_JSON_SOURCE_MODE} (expected fetch|system)")
endif()

# Resolve CLI11 source strategy
#   fetch (default): FetchContent from upstream release tag into CMake _deps
#   submodule: use code/thirdparty/cli11 only for intentionally customized/forked dependency flows
#   system: use find_package(CLI11)
set(KOG_CLI11_SOURCE_MODE "fetch" CACHE STRING "CLI11 source mode: fetch|submodule|system")
set_property(CACHE KOG_CLI11_SOURCE_MODE PROPERTY STRINGS fetch submodule system)

set(KOG_CLI11_SUBMODULE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/code/thirdparty/cli11")
set(KOG_CLI11_BINARY_DIR "${CMAKE_BINARY_DIR}/thirdparty/cli11-build")

if(KOG_CLI11_SOURCE_MODE STREQUAL "fetch")
    kog_fetchcontent_declare_git(
        cli11
        REPOSITORY https://github.com/CLIUtils/CLI11.git
        TAG v2.6.2
    )
    set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CLI11_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CLI11_PRECOMPILED OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(cli11)
    set(cli11_SOURCE_DIR "${cli11_SOURCE_DIR}")
elseif(KOG_CLI11_SOURCE_MODE STREQUAL "submodule")
    if(EXISTS "${KOG_CLI11_SUBMODULE_SOURCE_DIR}/CMakeLists.txt")
        add_subdirectory("${KOG_CLI11_SUBMODULE_SOURCE_DIR}" "${KOG_CLI11_BINARY_DIR}" EXCLUDE_FROM_ALL)
        set(cli11_SOURCE_DIR "${KOG_CLI11_SUBMODULE_SOURCE_DIR}")
    else()
        message(FATAL_ERROR "CLI11 submodule missing: ${KOG_CLI11_SUBMODULE_SOURCE_DIR}. Run: git submodule update --init --recursive")
    endif()
elseif(KOG_CLI11_SOURCE_MODE STREQUAL "system")
    find_package(CLI11 CONFIG REQUIRED)
else()
    message(FATAL_ERROR "Unsupported KOG_CLI11_SOURCE_MODE: ${KOG_CLI11_SOURCE_MODE} (expected fetch|submodule|system)")
endif()

set(KOG_FTXUI_SUBMODULE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/code/thirdparty/ftxui")
set(KOG_FTXUI_BINARY_DIR "${CMAKE_BINARY_DIR}/thirdparty/ftxui-build")
set(KOG_FTXUI_SOURCE_MODE "fetch" CACHE STRING "FTXUI source mode: fetch|submodule")
set_property(CACHE KOG_FTXUI_SOURCE_MODE PROPERTY STRINGS fetch submodule)

if(KOG_FTXUI_SOURCE_MODE STREQUAL "fetch")
    kog_fetchcontent_declare_git(
        ftxui
        REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
        TAG v5.0.0
    )
    FetchContent_MakeAvailable(ftxui)
    message(STATUS "FTXUI fetched into CMake _deps: enabling full tui")
elseif(KOG_FTXUI_SOURCE_MODE STREQUAL "submodule")
    if(EXISTS "${KOG_FTXUI_SUBMODULE_SOURCE_DIR}/CMakeLists.txt")
        add_subdirectory("${KOG_FTXUI_SUBMODULE_SOURCE_DIR}" "${KOG_FTXUI_BINARY_DIR}" EXCLUDE_FROM_ALL)
        message(STATUS "FTXUI submodule found: enabling full tui")
    else()
        message(FATAL_ERROR "FTXUI submodule missing: ${KOG_FTXUI_SUBMODULE_SOURCE_DIR}. Run: git submodule update --init --recursive")
    endif()
else()
    message(FATAL_ERROR "Unsupported KOG_FTXUI_SOURCE_MODE: ${KOG_FTXUI_SOURCE_MODE} (expected fetch|submodule)")
endif()

# Resolve toml++ source strategy
#   submodule: use code/thirdparty/tomlplusplus from git submodule
#   fetch (default): FetchContent from upstream release tag
#   system: use find_package(tomlplusplus)
set(KOG_TOMLPP_SOURCE_MODE "fetch" CACHE STRING "toml++ source mode: submodule|fetch|system")
set_property(CACHE KOG_TOMLPP_SOURCE_MODE PROPERTY STRINGS submodule fetch system)

set(KOG_TOMLPP_SUBMODULE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/code/thirdparty/tomlplusplus")
set(KOG_TOMLPP_BINARY_DIR "${CMAKE_BINARY_DIR}/thirdparty/tomlplusplus-build")

if(KOG_TOMLPP_SOURCE_MODE STREQUAL "submodule")
    if(EXISTS "${KOG_TOMLPP_SUBMODULE_SOURCE_DIR}/CMakeLists.txt")
        add_subdirectory("${KOG_TOMLPP_SUBMODULE_SOURCE_DIR}" "${KOG_TOMLPP_BINARY_DIR}" EXCLUDE_FROM_ALL)
    else()
        message(FATAL_ERROR "toml++ submodule missing: ${KOG_TOMLPP_SUBMODULE_SOURCE_DIR}. Run: git submodule update --init --recursive")
    endif()
elseif(KOG_TOMLPP_SOURCE_MODE STREQUAL "fetch")
    kog_fetchcontent_declare_git(
        tomlplusplus
        REPOSITORY https://github.com/marzer/tomlplusplus.git
        TAG v3.4.0
    )
    set(TOMLPLUSPLUS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(TOMLPLUSPLUS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(TOMLPLUSPLUS_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(tomlplusplus)
elseif(KOG_TOMLPP_SOURCE_MODE STREQUAL "system")
    find_package(tomlplusplus CONFIG REQUIRED)
else()
    message(FATAL_ERROR "Unsupported KOG_TOMLPP_SOURCE_MODE: ${KOG_TOMLPP_SOURCE_MODE} (expected submodule|fetch|system)")
endif()


# Platform detection
if(WIN32)
    add_compile_definitions(KOG_PLATFORM_WINDOWS=1)
    add_compile_definitions(NOMINMAX)
else()
    add_compile_definitions(KOG_PLATFORM_UNIX=1)
endif()

# Output directories
set(KOG_BUILD_ROOT "${CMAKE_SOURCE_DIR}/out")
set(KOG_PRESET_NAME "unknown-preset" CACHE STRING "CMake preset name for artifact layout")

set(KOG_BIN_ROOT "${KOG_BUILD_ROOT}/bin/${KOG_PRESET_NAME}")
set(KOG_LIB_ROOT "${KOG_BUILD_ROOT}/lib/${KOG_PRESET_NAME}")
set(KOG_SYMBOL_ROOT "${KOG_BUILD_ROOT}/obj/${KOG_PRESET_NAME}/symbols")

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

# =============================================================================
# Coverage and Profile-Guided Optimization (PGO) Options
# =============================================================================
option(KOG_ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)
set(KOG_PGO_MODE "off" CACHE STRING "PGO mode: off|collect|use")
set_property(CACHE KOG_PGO_MODE PROPERTY STRINGS off collect use)
set(KOG_PGO_LTCG "auto" CACHE STRING "PGO LTCG/LTO: auto|off (auto enables LTCG when PGO_MODE=use)")
set_property(CACHE KOG_PGO_LTCG PROPERTY STRINGS auto off)

# Coverage flags
# NOTE: For Clang, we use PGO-style instrumentation (-fprofile-instr-generate -fcoverage-mapping)
# which produces .profraw files. This unifies coverage and PGO collect: one test run generates
# data for both coverage report AND PGO profile. llvm-cov is used for reporting.
# For GCC, we fall back to traditional -fprofile-arcs -ftest-coverage (gcov style).
# For MSVC, /PROFILE enables coverage-oriented instrumentation only; it is not MSVC PGO training data.
if(KOG_ENABLE_COVERAGE)
    if(MSVC)
        # MSVC /PROFILE is a linker option for coverage/profiler scenarios.
        # MSVC PGO training data is produced by /GENPROFILE (or /FASTGENPROFILE), then consumed by /USEPROFILE.
        add_link_options(/PROFILE)
        set(KOG_COVERAGE_OUTPUT_DIR "${KOG_BUILD_ROOT}/coverage" CACHE PATH "Coverage output directory")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Clang-native PGO-style instrumentation: works for both coverage AND PGO collect
        # NOTE: Do NOT add -fno-exceptions - the codebase uses exceptions
        add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
        add_link_options(-fprofile-instr-generate -fcoverage-mapping)
        set(KOG_COVERAGE_OUTPUT_DIR "${KOG_BUILD_ROOT}/coverage" CACHE PATH "Coverage output directory")
        message(STATUS "Coverage: Clang PGO-style (llvm-cov reporting, unified with PGO collect)")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        # GCC traditional coverage
        add_compile_options(-fprofile-arcs -ftest-coverage -fno-exceptions)
        add_link_options(-fprofile-arcs -ftest-coverage)
        set(KOG_COVERAGE_OUTPUT_DIR "${KOG_BUILD_ROOT}/coverage" CACHE PATH "Coverage output directory")
        message(STATUS "Coverage: GCC traditional (gcov reporting)")
    else()
        message(WARNING "Coverage not supported for compiler: ${CMAKE_CXX_COMPILER_ID}")
    endif()
    message(STATUS "Coverage enabled (output: ${KOG_COVERAGE_OUTPUT_DIR})")
endif()

# PGO flags
if(NOT KOG_PGO_MODE STREQUAL "off")
    if(MSVC)
        if(KOG_PGO_MODE STREQUAL "collect")
            # Collect profile data using /GL (whole program optimization) + /LTCG /GENPROFILE (instrumented build)
            # /GL enables whole program optimization for all compiled modules
            # /LTCG /GENPROFILE creates instrumented build that generates .pgd file on execution
            add_compile_options(/GL)
            # Keep incremental mode consistent between collect/use to avoid LNK1268.
            add_link_options(/LTCG /GENPROFILE /INCREMENTAL:NO)
            message(STATUS "PGO: collect mode (MSVC /GL /LTCG /GENPROFILE - run tests to generate .pgd)")
        elseif(KOG_PGO_MODE STREQUAL "use")
            # Use existing profile data with /GL (whole program optimization) + /LTCG /USEPROFILE
            # /USEPROFILE replaces the deprecated /PROFILE:Optimize
            # PGO use mode automatically enables /LTCG for better optimization
            add_compile_options(/GL)
            if(NOT KOG_PGO_LTCG STREQUAL "off")
                add_link_options(/LTCG /USEPROFILE)
                message(STATUS "PGO: use mode with LTCG (MSVC /GL /LTCG /USEPROFILE)")
            else()
                add_link_options(/LTCG /USEPROFILE)
                message(STATUS "PGO: use mode (MSVC /GL /LTCG /USEPROFILE)")
            endif()
        endif()
        set(KOG_PGO_PROFILE_DIR "${KOG_BUILD_ROOT}/pgo" CACHE PATH "PGO profile data directory")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        if(KOG_PGO_MODE STREQUAL "collect")
            # GCC: instrument with -fprofile-generate
            add_compile_options(-fprofile-generate)
            add_link_options(-fprofile-generate)
            message(STATUS "PGO: collect mode (GCC -fprofile-generate)")
        elseif(KOG_PGO_MODE STREQUAL "use")
            # GCC: use profile with -fprofile-use
            # PGO use mode automatically enables LTO for better optimization
            if(NOT KOG_PGO_LTCG STREQUAL "off")
                add_compile_options(-fprofile-use -flto -fno-peel-loops -fno-tracer)
                add_link_options(-fprofile-use -flto)
                message(STATUS "PGO: use mode with LTO (GCC -fprofile-use -flto)")
            else()
                add_compile_options(-fprofile-use -fno-peel-loops -fno-tracer)
                add_link_options(-fprofile-use)
                message(STATUS "PGO: use mode (GCC -fprofile-use)")
            endif()
        endif()
        set(KOG_PGO_PROFILE_DIR "${KOG_BUILD_ROOT}/pgo" CACHE PATH "PGO profile data directory")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        if(KOG_PGO_MODE STREQUAL "collect")
            # Clang: instrument with -fprofile-instr-generate
            add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
            add_link_options(-fprofile-instr-generate -fcoverage-mapping)
            message(STATUS "PGO: collect mode (Clang -fprofile-instr-generate)")
        elseif(KOG_PGO_MODE STREQUAL "use")
            # Clang: use profile with -fprofile-instr-use=<path>
            # Set KOG_PGO_PROFILE_DIR first, then use it for the profile path
            set(KOG_PGO_PROFILE_DIR "${KOG_BUILD_ROOT}/pgo" CACHE PATH "PGO profile data directory")
            set(LLVM_PROFILE_FILE "${KOG_PGO_PROFILE_DIR}/merged.profdata")
            # Pass profile path directly to compiler via CMAKE_CXX_FLAGS
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-instr-use=${LLVM_PROFILE_FILE}")
            if(NOT KOG_PGO_LTCG STREQUAL "off")
                add_compile_options(-flto=thin)
                add_link_options(-flto=thin)
                message(STATUS "PGO: use mode with thin LTO (Clang -fprofile-instr-use=${LLVM_PROFILE_FILE})")
            else()
                message(STATUS "PGO: use mode (Clang -fprofile-instr-use=${LLVM_PROFILE_FILE})")
            endif()
        endif()
    else()
        message(WARNING "PGO not supported for compiler: ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

function(kog_configure_kano_module target_name)
  target_compile_features(${target_name} PUBLIC cxx_std_23)
  if(KOG_ENABLE_MODULES)
    set_target_properties(${target_name} PROPERTIES CXX_SCAN_FOR_MODULES ON)
    target_compile_definitions(${target_name} PUBLIC KOG_USE_MODULES=1)
  else()
    set_target_properties(${target_name} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  endif()
  target_compile_definitions(${target_name} PRIVATE CLI11_HAS_CODECVT=0)
endfunction()

set(KANO_GIT_CORE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/code/systems/kano_git_core")
set(KANO_GIT_COMMAND_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/code/systems/kano_git_command")

set(KOG_INFRA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shared/infra")
if(EXISTS "${KOG_INFRA_SOURCE_DIR}/CMakeLists.txt")
    add_subdirectory("${KOG_INFRA_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/shared/infra")
    if(TARGET kano_infra_build_info)
        target_compile_definitions(kano_infra_build_info PRIVATE
            KANO_INFRA_BUILD_VERSION="${KOG_BUILD_VERSION_STR}"
            KANO_INFRA_BUILD_REVISION="${KOG_BUILD_REVISION_STR}"
            KANO_INFRA_BUILD_BRANCH="${KOG_BUILD_BRANCH_STR}"
            KANO_INFRA_BUILD_TYPE="${KOG_BUILD_CONFIGURATION_STR}"
            KANO_INFRA_BUILD_COMPILER="${KOG_BUILD_TOOLCHAIN_STR}"
            KANO_INFRA_BUILD_TIMESTAMP="unknown"
            KANO_INFRA_BUILD_VCS_STATUS="${KOG_BUILD_DIRTY_STR}"
        )
    endif()
endif()

# Add subdirectories
add_subdirectory(code/systems/kano_git_core)
add_subdirectory(code/systems/kano_git_command)
add_subdirectory(code/apps/kano_git_cli)
add_subdirectory(code/apps/kano_git_tui)

option(KOG_BUILD_TESTS "Build kano-git C++ test targets" ON)
if(KOG_BUILD_TESTS)
    add_subdirectory(code/tests)
endif()

kog_apply_unity_configuration()
