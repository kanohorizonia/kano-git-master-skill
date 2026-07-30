add_library(kano_git_audit STATIC)

target_sources(kano_git_audit
  PRIVATE
    private/audit_contract.cpp
  PUBLIC
    public/audit_contract.hpp
)

kog_configure_kano_module(kano_git_audit)

target_include_directories(kano_git_audit
  PUBLIC
    ${KANO_GIT_CORE_ROOT}/audit/public
  PRIVATE
    ${KANO_GIT_CORE_ROOT}/audit/private
)

target_link_libraries(kano_git_audit
  PRIVATE
    nlohmann_json::nlohmann_json
)

add_library(kano::git_audit ALIAS kano_git_audit)
