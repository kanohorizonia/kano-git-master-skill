if(WIN32)
  add_custom_target(run_plan_commit_regression_e2e
    COMMAND pwsh -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/plan_commit_regression/run.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running kano-git plan/commit regression e2e"
  )
else()
  add_custom_target(run_plan_commit_regression_e2e
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/plan_commit_regression/run.sh"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running kano-git plan/commit regression e2e"
  )
endif()

add_custom_target(run_kano_git_e2e
  DEPENDS run_plan_commit_regression_e2e
  COMMENT "Running kano-git e2e regressions"
)
