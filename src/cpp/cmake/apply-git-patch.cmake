foreach(required_variable
        KOG_PATCH_SOURCE_DIR
        KOG_PATCH_FILE
        KOG_GIT_EXECUTABLE)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env "GIT_MASTER=1"
        "${KOG_GIT_EXECUTABLE}" -C "${KOG_PATCH_SOURCE_DIR}" apply
        --reverse --check --ignore-space-change
        "${KOG_PATCH_FILE}"
    RESULT_VARIABLE patch_already_applied
    OUTPUT_QUIET
    ERROR_QUIET
)
if(patch_already_applied EQUAL 0)
    message(STATUS "Patch already applied: ${KOG_PATCH_FILE}")
    return()
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env "GIT_MASTER=1"
        "${KOG_GIT_EXECUTABLE}" -C "${KOG_PATCH_SOURCE_DIR}" apply
        --whitespace=nowarn --ignore-space-change
        "${KOG_PATCH_FILE}"
    RESULT_VARIABLE patch_result
    OUTPUT_VARIABLE patch_output
    ERROR_VARIABLE patch_error
)
if(NOT patch_result EQUAL 0)
    string(STRIP "${patch_output}" patch_output)
    string(STRIP "${patch_error}" patch_error)
    message(FATAL_ERROR
        "Failed to apply ${KOG_PATCH_FILE}\n"
        "stdout: ${patch_output}\n"
        "stderr: ${patch_error}")
endif()

message(STATUS "Applied patch: ${KOG_PATCH_FILE}")
