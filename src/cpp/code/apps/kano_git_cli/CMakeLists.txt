add_executable(kano_git_cli
    main.cpp
)

set_target_properties(kano_git_cli PROPERTIES OUTPUT_NAME kano-git)

target_link_libraries(kano_git_cli PRIVATE kano::git_core kano::git_command kano::git_build_info)

set(KOG_RUNTIME_ARTIFACT_DIR "${CMAKE_BINARY_DIR}/runtime-artifact" CACHE PATH "Minimal KOG runtime artifact output directory")
set(KOG_RUNTIME_MANIFEST_INTERMEDIATE "${CMAKE_CURRENT_BINARY_DIR}/kog-runtime-manifest.configured.json")
set(KOG_RUNTIME_MANIFEST_CONFIGURED "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/kog-runtime-manifest.json")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime-manifest.json.in"
    "${KOG_RUNTIME_MANIFEST_INTERMEDIATE}"
    @ONLY
)
file(GENERATE OUTPUT "${KOG_RUNTIME_MANIFEST_CONFIGURED}" INPUT "${KOG_RUNTIME_MANIFEST_INTERMEDIATE}")

set(_kog_runtime_artifact_commands
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${KOG_RUNTIME_ARTIFACT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${KOG_RUNTIME_ARTIFACT_DIR}/bin"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${KOG_RUNTIME_ARTIFACT_DIR}/lib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:kano_git_cli>" "${KOG_RUNTIME_ARTIFACT_DIR}/bin/$<TARGET_FILE_NAME:kano_git_cli>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KOG_RUNTIME_MANIFEST_CONFIGURED}" "${KOG_RUNTIME_ARTIFACT_DIR}/manifest.json"
)

if(UNIX AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
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
    list(APPEND _kog_runtime_artifact_commands
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_kog_libstdcpp}" "${KOG_RUNTIME_ARTIFACT_DIR}/lib/libstdc++.so.6"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_kog_libgcc}" "${KOG_RUNTIME_ARTIFACT_DIR}/lib/libgcc_s.so.1"
    )
endif()

add_custom_target(kog_runtime_artifact
    ${_kog_runtime_artifact_commands}
    DEPENDS kano_git_cli
    COMMENT "Packaging minimal KOG provider runtime artifact"
    VERBATIM
)
