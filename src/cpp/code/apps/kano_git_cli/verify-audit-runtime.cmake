cmake_minimum_required(VERSION 3.24)

foreach(required_var
        KOG_AUDIT_RUNTIME_ARTIFACT_DIR
        KOG_AUDIT_SCHEMA_ROOT)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} is required")
    endif()
endforeach()

set(manifest_path "${KOG_AUDIT_RUNTIME_ARTIFACT_DIR}/manifest.json")
set(binary_path "${KOG_AUDIT_RUNTIME_ARTIFACT_DIR}/bin/kano-git")
if(NOT EXISTS "${manifest_path}")
    message(FATAL_ERROR "runtime manifest is missing: ${manifest_path}")
endif()
if(NOT EXISTS "${binary_path}")
    message(FATAL_ERROR "runtime binary is missing: ${binary_path}")
endif()

file(READ "${manifest_path}" manifest_json)
string(JSON runtime_asset_count
    ERROR_VARIABLE manifest_error
    LENGTH "${manifest_json}" runtime_assets)
if(manifest_error)
    message(FATAL_ERROR
        "runtime manifest has no valid runtime_assets array: ${manifest_error}")
endif()
if(runtime_asset_count LESS 1)
    message(FATAL_ERROR "runtime manifest declares no runtime assets")
endif()
math(EXPR runtime_asset_last "${runtime_asset_count} - 1")

foreach(schema_name
        kog.auditEvent.v1.schema.json
        kog.runReceipt.v1.schema.json)
    set(relative_path "assets/audit/schemas/${schema_name}")
    set(source_path "${KOG_AUDIT_SCHEMA_ROOT}/${schema_name}")
    set(packaged_path
        "${KOG_AUDIT_RUNTIME_ARTIFACT_DIR}/${relative_path}")
    if(NOT EXISTS "${source_path}")
        message(FATAL_ERROR "source audit schema is missing: ${source_path}")
    endif()
    if(NOT EXISTS "${packaged_path}")
        message(FATAL_ERROR "packaged audit schema is missing: ${packaged_path}")
    endif()

    set(asset_declared FALSE)
    foreach(asset_index RANGE 0 ${runtime_asset_last})
        string(JSON candidate
            ERROR_VARIABLE asset_error
            GET "${manifest_json}" runtime_assets ${asset_index})
        if(asset_error)
            message(FATAL_ERROR
                "runtime_assets[${asset_index}] is invalid: ${asset_error}")
        endif()
        if(candidate STREQUAL relative_path)
            set(asset_declared TRUE)
        endif()
    endforeach()
    if(NOT asset_declared)
        message(FATAL_ERROR "runtime manifest omits ${relative_path}")
    endif()

    file(SHA256 "${source_path}" source_sha256)
    file(SHA256 "${packaged_path}" packaged_sha256)
    if(NOT source_sha256 STREQUAL packaged_sha256)
        message(FATAL_ERROR
            "packaged audit schema differs from source: ${relative_path}")
    endif()
endforeach()

file(SIZE "${binary_path}" binary_size)
if(binary_size EQUAL 0)
    message(FATAL_ERROR "runtime binary is empty: ${binary_path}")
endif()

message(STATUS
    "Audit runtime artifact manifest, schema bytes, and binary passed")
