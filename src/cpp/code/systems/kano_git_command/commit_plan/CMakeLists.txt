add_library(kano_git_cmd_commit_plan STATIC)

target_sources(kano_git_cmd_commit_plan
  PRIVATE
    private/agent_queue_cmd.cpp
    private/agent_queue_cmd.hpp
    private/commit_ai_utils.cpp
    private/commit_lock_recovery.cpp
    private/commit_lock_recovery.hpp
    private/commit_auto_plan_pipeline.cpp
    private/commit_auto_plan_pipeline.hpp
    private/commit_cmd.cpp
    private/commit_plan_payload.cpp
    private/commit_plan_audit.cpp
    private/commit_plan_execution_audit.cpp
    private/commit_plan_execution_audit.hpp
    private/commit_plan_execution_admission.cpp
    private/commit_plan_execution_admission.hpp
    private/commit_plan_source_rewrite.cpp
    private/commit_plan_source_rewrite.hpp
    private/commit_plan_path_scope.cpp
    private/commit_plan_path_scope.hpp
    private/commit_plan_payload.hpp
    private/commit_plan_payload_parser.hpp
    private/commit_plan_task_graph.cpp
    private/commit_plan_task_graph.hpp
    private/commit_utils.cpp
    private/commit_utils.hpp
    private/commit_push_cli.cpp
    private/commit_push_cmd.cpp
    private/commit_push_cmd.hpp
    private/commit_push_post_sync.cpp
    private/commit_push_post_sync.hpp
    private/cherry_pick_cmd.cpp
    private/doctor_cmd.cpp
    private/ignore_cmd.cpp
    private/plan_cmd.cpp
    private/plan_utils.cpp
    private/resolve_cmd.cpp
)

kog_configure_kano_module(kano_git_cmd_commit_plan)

target_include_directories(kano_git_cmd_commit_plan
  PRIVATE
    ${KANO_GIT_COMMAND_ROOT}/commit_plan/private
)

target_link_libraries(kano_git_cmd_commit_plan
  PUBLIC kano::git_shell kano::git_workspace kano::git_command_runtime KanoInfra::timing nlohmann_json::nlohmann_json
  PRIVATE kano::git_audit kano::git_cmd_repo_sync
)

add_library(kano::git_cmd_commit_plan ALIAS kano_git_cmd_commit_plan)
