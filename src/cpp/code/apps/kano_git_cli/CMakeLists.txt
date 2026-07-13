add_executable(kano_git_cli
    main.cpp
)

set_target_properties(kano_git_cli PROPERTIES OUTPUT_NAME kano-git)

target_link_libraries(kano_git_cli PRIVATE kano::git_core kano::git_command kano::git_build_info)

set(KOG_RUNTIME_ARTIFACT_DIR "${CMAKE_BINARY_DIR}/runtime-artifact" CACHE PATH "Minimal KOG runtime artifact output directory")
set(KOG_RUNTIME_MANIFEST_INTERMEDIATE "${CMAKE_CURRENT_BINARY_DIR}/kog-runtime-manifest.configured.json")
set(KOG_RUNTIME_MANIFEST_CONFIGURED "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/kog-runtime-manifest.json")
set(KOG_RUNTIME_PACKAGER "${CMAKE_CURRENT_SOURCE_DIR}/package-runtime.cmake")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime-manifest.json.in"
    "${KOG_RUNTIME_MANIFEST_INTERMEDIATE}"
    @ONLY
)
file(GENERATE OUTPUT "${KOG_RUNTIME_MANIFEST_CONFIGURED}" INPUT "${KOG_RUNTIME_MANIFEST_INTERMEDIATE}")

set(_kog_runtime_shared_library_files)
if(UNIX AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    foreach(_kog_runtime_target
            kano_infra_timing
            kano_infra_config
            kano_infra_build_info
            kano_infra_platform
            kano_infra_process
            kano_infra_diagnostics
            kano_infra_self)
        list(APPEND _kog_runtime_shared_library_files "$<TARGET_FILE:${_kog_runtime_target}>")
    endforeach()
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libstdc++.so.6
        OUTPUT_VARIABLE _kog_libstdcpp
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libgcc_s.so.1
        OUTPUT_VARIABLE _kog_libgcc
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
    if(NOT EXISTS "${_kog_libstdcpp}" OR NOT EXISTS "${_kog_libgcc}")
        message(FATAL_ERROR "GNU runtime libraries required by kog_runtime_artifact were not found")
    endif()
endif()
list(JOIN _kog_runtime_shared_library_files "|" _kog_runtime_shared_library_files_arg)

add_custom_target(kog_runtime_artifact
    COMMAND ${CMAKE_COMMAND}
        "-DKOG_RUNTIME_ARTIFACT_DIR=${KOG_RUNTIME_ARTIFACT_DIR}"
        "-DKOG_RUNTIME_BINARY=$<TARGET_FILE:kano_git_cli>"
        "-DKOG_RUNTIME_MANIFEST=${KOG_RUNTIME_MANIFEST_CONFIGURED}"
        "-DKOG_RUNTIME_SHARED_LIBRARIES=${_kog_runtime_shared_library_files_arg}"
        "-DKOG_RUNTIME_LIBSTDCPP=${_kog_libstdcpp}"
        "-DKOG_RUNTIME_LIBGCC=${_kog_libgcc}"
        -P "${KOG_RUNTIME_PACKAGER}"
    DEPENDS kano_git_cli
    COMMENT "Packaging minimal KOG provider runtime artifact"
    VERBATIM
)
