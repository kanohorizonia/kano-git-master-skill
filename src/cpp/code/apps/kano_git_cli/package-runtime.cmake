cmake_minimum_required(VERSION 3.24)

foreach(required_var
        KOG_RUNTIME_ARTIFACT_DIR
        KOG_RUNTIME_BINARY
        KOG_RUNTIME_MANIFEST)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} is required")
    endif()
endforeach()

foreach(required_file KOG_RUNTIME_BINARY KOG_RUNTIME_MANIFEST)
    if(NOT EXISTS "${${required_file}}")
        message(FATAL_ERROR "${required_file} does not exist: ${${required_file}}")
    endif()
endforeach()

file(REMOVE_RECURSE "${KOG_RUNTIME_ARTIFACT_DIR}")
file(MAKE_DIRECTORY
    "${KOG_RUNTIME_ARTIFACT_DIR}/bin"
    "${KOG_RUNTIME_ARTIFACT_DIR}/lib")
file(COPY_FILE
    "${KOG_RUNTIME_BINARY}"
    "${KOG_RUNTIME_ARTIFACT_DIR}/bin/kano-git"
    ONLY_IF_DIFFERENT)
file(COPY_FILE
    "${KOG_RUNTIME_MANIFEST}"
    "${KOG_RUNTIME_ARTIFACT_DIR}/manifest.json"
    ONLY_IF_DIFFERENT)

string(REPLACE "|" ";" runtime_shared_libraries "${KOG_RUNTIME_SHARED_LIBRARIES}")
foreach(runtime_library IN LISTS runtime_shared_libraries)
    if(NOT EXISTS "${runtime_library}")
        message(FATAL_ERROR "KOG shared-library target output does not exist: ${runtime_library}")
    endif()
    get_filename_component(runtime_library_name "${runtime_library}" NAME)
    file(COPY_FILE
        "${runtime_library}"
        "${KOG_RUNTIME_ARTIFACT_DIR}/lib/${runtime_library_name}"
        ONLY_IF_DIFFERENT)
endforeach()

foreach(compiler_runtime KOG_RUNTIME_LIBSTDCPP KOG_RUNTIME_LIBGCC)
    if(DEFINED ${compiler_runtime} AND NOT "${${compiler_runtime}}" STREQUAL "")
        if(NOT EXISTS "${${compiler_runtime}}")
            message(FATAL_ERROR "${compiler_runtime} does not exist: ${${compiler_runtime}}")
        endif()
        get_filename_component(compiler_runtime_name "${${compiler_runtime}}" NAME)
        file(COPY_FILE
            "${${compiler_runtime}}"
            "${KOG_RUNTIME_ARTIFACT_DIR}/lib/${compiler_runtime_name}"
            ONLY_IF_DIFFERENT)
    endif()
endforeach()

if(CMAKE_HOST_UNIX)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "LD_LIBRARY_PATH=${KOG_RUNTIME_ARTIFACT_DIR}/lib"
            ldd "${KOG_RUNTIME_ARTIFACT_DIR}/bin/kano-git"
        RESULT_VARIABLE ldd_result
        OUTPUT_VARIABLE ldd_output
        ERROR_VARIABLE ldd_error)
    if(NOT ldd_result EQUAL 0 OR ldd_output MATCHES "not found" OR ldd_error MATCHES "not found")
        message(FATAL_ERROR
            "KOG runtime artifact has unresolved shared libraries\n${ldd_output}${ldd_error}")
    endif()
endif()
