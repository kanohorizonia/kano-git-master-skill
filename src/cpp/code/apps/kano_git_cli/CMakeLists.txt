add_executable(kano_git_cli
    main.cpp
)

set_target_properties(kano_git_cli PROPERTIES OUTPUT_NAME kano-git)

target_include_directories(kano_git_cli PRIVATE ../../systems/kano_git_core/public)
target_link_libraries(kano_git_cli PRIVATE kano::git_core)
